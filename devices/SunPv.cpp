// SunPv 光伏逆变器 Modbus 通信驱动模块
// 功能：通过 Modbus RTU/TCP 与光伏逆变器通信，采集运行状态、告警信息，
//       并支持有功功率限制指令下发。支持多台逆变器轮询。

#include "SunPv.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

// ========== 逆变器状态枚举 ==========
// 定义光伏逆变器的各种工作状态，对应设备协议中的 16 位状态码
enum class SunPvInverterState : uint16_t {
    Running          = 0x0000,  // 正常运行
    Stop             = 0x8000,  // 停止
    ButtonStop       = 0x1300,  // 按键停止
    EmergencyStop    = 0x1500,  // 急停
    Standby          = 0x1400,  // 待机
    InitialStandby   = 0x1200,  // 初始待机
    Startup          = 0x1600,  // 启动中
    AlarmRunning     = 0x9100,  // 告警运行（有告警但仍运行）
    DeratingRunning  = 0x8100,  // 降额运行
    SchedulingRunning = 0x8200, // 调度运行
    Fault            = 0x5500,  // 故障
    CommunicationFault = 0x2500, // 通信故障
    Uninitialized    = 0x1111,  // 未初始化
};

// ========== 16 位数值范围结构 ==========
// 用于定义告警码的连续区间
struct U16Range {
    uint16_t lo;  // 区间下界（含）
    uint16_t hi;  // 区间上界（含）
};

// ========== 告警5044 位域结构 ==========
// 将 Modbus 寄存器 5044 的 32 个告警位展开为独立的布尔字段，
// 每个字段对应一种具体的告警/故障类型，方便上层代码按语义访问。
struct Alarm5044 {
    bool GridOvervoltage           = false;  // 电网过电压
    bool GridUndervoltage          = false;  // 电网欠电压
    bool GridOverfrequency         = false;  // 电网过频率
    bool GridUnderfrequency        = false;  // 电网欠频率
    bool GridPowerOutage           = false;  // 电网停电
    bool LeakageCurrentHigh        = false;  // 漏电流过高
    bool GridAbnormality           = false;  // 电网异常
    bool GridVoltageUnbalance      = false;  // 电网电压不平衡
    bool PvBackupConnectionFault   = false;  // 光伏备用连接故障
    bool PvReverseConnectionAlarm  = false;  // 光伏反接告警
    bool PvAbnormalityAlarm        = false;  // 光伏异常告警
    bool MpptReverseConnection     = false;  // MPPT反接
    bool BoostCapOvervoltageAlarm  = false;  // Boost电容过压告警
    bool BoostCapOvervoltageFault  = false;  // Boost电容过压故障
    bool PvCableShortToGround      = false;  // 光伏线缆对地短路
    bool StringCurrentBackflow     = false;  // 组串电流反流
    bool PvGroundingFault          = false;  // 光伏接地故障
    bool SystemHardwareFault       = false;  // 系统硬件故障
    bool AmbientTempHigh           = false;  // 环境温度过高
    bool AmbientTempLow            = false;  // 环境温度过低
    bool InsulationResistanceLow   = false;  // 绝缘电阻低
    bool GroundCableFault          = false;  // 接地线缆故障
    bool ArcFault                  = false;  // 电弧故障
    bool MeterCtReverseAlarm       = false;  // 电表CT反接告警
    bool MeterCommAbnormalityAlarm = false;  // 电表通信异常告警
    bool GridConflict              = false;  // 电网冲突
    bool InverterGridTiedCommAlarm = false;  // 逆变器并网通信告警
    bool SystemFault               = false;  // 系统故障（通用，由码表匹配）
    bool SystemAlarm               = false;  // 系统告警（通用，由码表匹配）
};

// ========== 辅助函数 ==========

// 判断 val 是否在 [lo, hi] 闭区间内
static bool inRange(uint16_t c, U16Range r) { return c >= r.lo && c <= r.hi; }

// 判断 val 是否在 tab[0..n-1] 中任意一个区间内
static bool inAnyRange(uint16_t c, const U16Range* tab, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (inRange(c, tab[i])) {
            return true;
        }
    }
    return false;
}

// ========== 系统故障码表 ==========
// 将 Modbus 寄存器中属于"系统故障"的码值（连续区间）列在此表中，
// 当告警码匹配到任一区间时，置位 SystemFault。
static const U16Range kSystemFaultRanges[] = {
    {7, 7},       {11, 11},   {16, 16},   {19, 25},   {30, 34},   {36, 36},
    {38, 38},     {40, 42},   {44, 50},   {52, 58},   {60, 68},   {85, 85},
    {87, 87},     {92, 92},   {93, 93},   {100, 105}, {107, 114}, {116, 124},
    {200, 207},   {209, 211}, {248, 255}, {300, 322}, {324, 326}, {401, 412},
    {600, 603},   {605, 605}, {608, 608}, {612, 612}, {616, 616}, {620, 620},
    {622, 624},   {681, 681}, {800, 800}, {802, 802}, {804, 804}, {807, 807},
    {1096, 1122},
};

// ========== 系统告警码表 ==========
// 将 Modbus 寄存器中属于"系统告警"的码值（连续区间）列在此表中，
// 当告警码匹配到任一区间时，置位 SystemAlarm。
static const U16Range kSystemAlarmRanges[] = {
    {59, 59},     {70, 72},   {74, 74},   {76, 76},   {77, 81},   {82, 82},
    {83, 83},     {86, 86},   {89, 89},   {216, 218}, {220, 231}, {396, 397},
    {432, 434},   {500, 513}, {515, 518}, {635, 638}, {900, 900}, {901, 901},
    {910, 910},   {911, 911}, {1124, 1127},
};

// ========== 告警解码函数 ==========
// 将 Modbus 寄存器 _Alarm5044 的 16 位码值解码为 Alarm5044 结构体中的
// 各布尔字段。每种告警码（或码区间）对应一个具体的告警类型。
// 参数 code: 寄存器 5044 的原始值，0 表示无告警
// 参数 out:   输出参数，解码后的告警位域
static void decodeAlarm5044(uint16_t code, Alarm5044& out)
{
    out = Alarm5044{};  // 清零所有告警位
    if (code == 0) {    // 无告警，直接返回
        return;
    }
    // 以下为各告警码 -> 具体告警类型的映射，匹配到即置位并返回
    if (code == 2 || code == 3 || code == 14 || code == 15) {
        out.GridOvervoltage = true;
        return;
    }
    if (code == 4 || code == 5) {
        out.GridUndervoltage = true;
        return;
    }
    if (code == 8) {
        out.GridOverfrequency = true;
        return;
    }
    if (code == 9) {
        out.GridUnderfrequency = true;
        return;
    }
    if (code == 10) {
        out.GridPowerOutage = true;
        return;
    }
    if (code == 12) {
        out.LeakageCurrentHigh = true;
        return;
    }
    if (code == 13) {
        out.GridAbnormality = true;
        return;
    }
    if (code == 17) {
        out.GridVoltageUnbalance = true;
        return;
    }
    if (code == 28 || code == 29 || code == 208 || inRange(code, {448, 479})) {
        out.PvBackupConnectionFault = true;
        return;
    }
    if (inRange(code, {532, 547}) || inRange(code, {564, 579})) {
        out.PvReverseConnectionAlarm = true;
        return;
    }
    if (inRange(code, {548, 563}) || inRange(code, {580, 595})) {
        out.PvAbnormalityAlarm = true;
        return;
    }
    if (inRange(code, {264, 283})) {
        out.MpptReverseConnection = true;
        return;
    }
    if (inRange(code, {332, 363})) {
        out.BoostCapOvervoltageAlarm = true;
        return;
    }
    if (inRange(code, {364, 395})) {
        out.BoostCapOvervoltageFault = true;
        return;
    }
    if (code == 1328) {
        out.PvCableShortToGround = true;
        return;
    }
    if (inRange(code, {1548, 1579})) {
        out.StringCurrentBackflow = true;
        return;
    }
    if (inRange(code, {1600, 1611})) {
        out.PvGroundingFault = true;
        return;
    }
    if (code == 1616) {
        out.SystemHardwareFault = true;
        return;
    }
    if (code == 37) {
        out.AmbientTempHigh = true;
        return;
    }
    if (code == 43) {
        out.AmbientTempLow = true;
        return;
    }
    if (code == 39) {
        out.InsulationResistanceLow = true;
        return;
    }
    if (code == 106) {
        out.GroundCableFault = true;
        return;
    }
    if (code == 88) {
        out.ArcFault = true;
        return;
    }
    if (code == 84) {
        out.MeterCtReverseAlarm = true;
        return;
    }
    if (code == 514) {
        out.MeterCommAbnormalityAlarm = true;
        return;
    }
    if (code == 323) {
        out.GridConflict = true;
        return;
    }
    if (code == 75) {
        out.InverterGridTiedCommAlarm = true;
        return;
    }
    // 通用系统故障码表匹配
    if (inAnyRange(code, kSystemFaultRanges, sizeof(kSystemFaultRanges) / sizeof(kSystemFaultRanges[0]))) {
        out.SystemFault = true;
        return;
    }
    // 通用系统告警码表匹配
    if (inAnyRange(code, kSystemAlarmRanges, sizeof(kSystemAlarmRanges) / sizeof(kSystemAlarmRanges[0]))) {
        out.SystemAlarm = true;
    }
}

// ========== 逆变器状态转换函数 ==========
// 将内部 SunPvInverterState 枚举值转换为 Modbus 协议规定的 4 种状态码：
//   0 = 停机/待机/未初始化/启动中
//   1 = 运行（含告警运行、降额运行、调度运行）
//   2 = 停止（含按键停止、急停）
//   3 = 故障（含硬件故障、通信故障）
// 返回: 对应的 Modbus 状态码（0/1/2/3）
static uint16_t mpptStatusFromState(uint16_t st)
{
    const auto eq = [st](SunPvInverterState s) { return st == static_cast<uint16_t>(s); };
    if (eq(SunPvInverterState::Running) || eq(SunPvInverterState::AlarmRunning)
        || eq(SunPvInverterState::DeratingRunning) || eq(SunPvInverterState::SchedulingRunning)) {
        return 1;  // 运行
    }
    if (eq(SunPvInverterState::Stop) || eq(SunPvInverterState::ButtonStop)
        || eq(SunPvInverterState::EmergencyStop)) {
        return 2;  // 停止
    }
    if (eq(SunPvInverterState::Standby) || eq(SunPvInverterState::InitialStandby)
        || eq(SunPvInverterState::Startup) || eq(SunPvInverterState::Uninitialized)) {
        return 0;  // 停机/待机
    }
    if (eq(SunPvInverterState::Fault) || eq(SunPvInverterState::CommunicationFault)) {
        return 3;  // 故障
    }
    return 0;  // 默认停机
}

}  // namespace

// ========== SunPv 类实现 ==========

// 构造函数：加载 Modbus 设备配置，初始化 RTU 通信和轮询引擎
// 参数 configPath: 配置文件路径，为空时使用 Config 默认路径
SunPv::SunPv(const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    ModbusDeviceProfile p = loadModbusDeviceProfile(path, Config::SUN_PV_DEVICE_ID);
    if (p.rtu_device.empty()) {
        throw std::runtime_error("sun_pv: rtu_device empty in " + path);
    }
    bus_ = std::make_unique<ModbusRTU>(p.rtu_device.c_str(), p.rtu_baud, p.timeout_ms);
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(p));
    engine_->setSkipProbe(true);
    // 设置解码后回调：每次 Modbus 数据解码完成后，调用 onAfterDecode 处理
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterDecode(eng, static_cast<MySQLDatabase&>(sink));
    });
    std::cout << "[sun_pv] rtu=" << engine_->profile().rtu_device << std::endl;
}

// 析构函数：断开 Modbus 连接
SunPv::~SunPv()
{
    if (bus_) {
        bus_->disconnect();
    }
}

// 帧间隔延时：在连续轮询多台逆变器时，按配置的 inter_frame_ms 延时，
// 避免串口总线冲突。
void SunPv::sleepFrameGap() const
{
    const int ms = engine_->profile().inter_frame_ms;
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

// 有功功率限制下发：计算单台逆变器的功率限制目标值，
// 通过 Modbus 寄存器 5007 写入。使用死区（deadband）机制避免频繁写入。
// 参数 eng: Modbus 轮询引擎引用
void SunPv::writePowerLimit(ModbusPollEngine& eng)
{
    // 未配置模块数或额定功率时不执行
    if (numberOfModules_ == 0 || kRatedPowerKw == 0) {
        return;
    }
    // 计算目标值：总功率限制 / 模块数 / 额定功率 * 1000（工程单位转换）
    const uint16_t target = static_cast<uint16_t>(std::round(
        static_cast<double>(himActivePowerLimit_) / numberOfModules_ / kRatedPowerKw * 1000.0));
    const int cur = static_cast<int>(eng.getValue("_ActivePowerLimit"));
    const int diff = static_cast<int>(target) - cur;
    constexpr int kDeadband = 10;  // 死区阈值，变化小于此值时不写入
    if (diff <= kDeadband && diff >= -kDeadband) {
        return;
    }
    bus_->writeRegister(5007, target, eng.profile().resolvedMysqlTable());
    std::cout << "SunPv有功功率限制指令下发:" << target << std::endl;
    LOG_ACTION("SunPv有功功率限制指令下发:" + std::to_string(target));
}

// 解码后回调：Modbus 数据解码完成后自动调用，
// 将原始寄存器值转换为语义化字段并写入数据库。
// 参数 eng: Modbus 轮询引擎引用
// 参数 db:  MySQL 数据库连接
void SunPv::onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db)
{
    (void)db;
    // 通信标志非零表示通信异常，跳过处理
    if (eng.commFlag() != 0) {
        return;
    }
    // 1. 转换并写入逆变器状态
    eng.setValue("MPPTStatus", static_cast<double>(
        mpptStatusFromState(static_cast<uint16_t>(eng.getValue("_InverterState")))));

    // 2. 解码告警 5044 并写入各告警字段
    Alarm5044 a;
    decodeAlarm5044(static_cast<uint16_t>(eng.getValue("_Alarm5044")), a);
    // 泛型 lambda：将告警布尔值转为 0/1 写入引擎
    auto setb = [&](const char* name, bool v) { eng.setValue(name, v ? 1.0 : 0.0); };
    setb("GridOvervoltage", a.GridOvervoltage);
    setb("GridUndervoltage", a.GridUndervoltage);
    setb("GridOverfrequency", a.GridOverfrequency);
    setb("GridUnderfrequency", a.GridUnderfrequency);
    setb("GridPowerOutage", a.GridPowerOutage);
    setb("LeakageCurrentHigh", a.LeakageCurrentHigh);
    setb("GridAbnormality", a.GridAbnormality);
    setb("GridVoltageUnbalance", a.GridVoltageUnbalance);
    setb("PvBackupConnectionFault", a.PvBackupConnectionFault);
    setb("PvReverseConnectionAlarm", a.PvReverseConnectionAlarm);
    setb("PvAbnormalityAlarm", a.PvAbnormalityAlarm);
    setb("MpptReverseConnection", a.MpptReverseConnection);
    setb("BoostCapOvervoltageAlarm", a.BoostCapOvervoltageAlarm);
    setb("BoostCapOvervoltageFault", a.BoostCapOvervoltageFault);
    setb("PvCableShortToGround", a.PvCableShortToGround);
    setb("StringCurrentBackflow", a.StringCurrentBackflow);
    setb("PvGroundingFault", a.PvGroundingFault);
    setb("SystemHardwareFault", a.SystemHardwareFault);
    setb("AmbientTempHigh", a.AmbientTempHigh);
    setb("AmbientTempLow", a.AmbientTempLow);
    setb("InsulationResistanceLow", a.InsulationResistanceLow);
    setb("GroundCableFault", a.GroundCableFault);
    setb("ArcFault", a.ArcFault);
    setb("MeterCtReverseAlarm", a.MeterCtReverseAlarm);
    setb("MeterCommAbnormalityAlarm", a.MeterCommAbnormalityAlarm);
    setb("GridConflict", a.GridConflict);
    setb("InverterGridTiedCommAlarm", a.InverterGridTiedCommAlarm);
    setb("SystemFault", a.SystemFault);
    setb("SystemAlarm", a.SystemAlarm);

    // 3. 根据 PID 码值（_PidCode）设置 PID 相关告警
    const uint16_t pid = static_cast<uint16_t>(eng.getValue("_PidCode"));
    setb("PIDImpedanceAbnormal", pid == 432);       // PID阻抗异常
    setb("PIDFunctionAbnormal", pid == 433);         // PID功能异常
    setb("PIDVoltageCurrentProtection", pid == 434); // PID电压电流保护

    // 4. 下发有功功率限制（如果配置了）
    writePowerLimit(eng);
}

// 轮询单台逆变器
// 参数 db:   MySQL 数据库连接
// 参数 amount: 逆变器编号（从 1 开始）
void SunPv::pollOneInverter(MySQLDatabase& db, int amount)
{
    const int idx = amount - 1;
    // 设置从站地址和表后缀，使引擎能正确映射寄存器到数据库表
    engine_->profile().slave = amount;
    engine_->setTableSuffix(amount);
    engine_->profile().comm.com_alarm_addr = kComAlarmAddrBase + idx;
    engine_->clearCaches();
    bus_->setSlave(amount);

    const std::string table = engine_->profile().resolvedMysqlTable();
    // 探测逆变器是否在线
    const bool ok = bus_->probe(engine_->profile().comm.probe_addr,
                                engine_->profile().comm.probe_count, &probeBuf_, table);
    sleepFrameGap();
    if (!ok) {
        // 通信失败，累计错误次数，超过 3 次标记为离线
        ++commErrCount_[static_cast<std::size_t>(idx)];
        const int offline = commErrCount_[static_cast<std::size_t>(idx)] > 3 ? 1 : 0;
        db.update(0, offline, table);
        db.update(engine_->profile().comm.com_alarm_addr, offline, "com_alarm");
        return;
    }

    // 通信正常，清零错误计数，执行一次完整轮询
    commErrCount_[static_cast<std::size_t>(idx)] = 0;
    engine_->setInheritCommFlag(0);
    engine_->pollOnce(db);
}

// 主循环线程：持续轮询所有逆变器，每秒（或配置的 poll_ms）执行一轮
// 参数 pool: MySQL 连接池
void SunPv::runSunPvThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));  // 启动延时
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 600;
    const auto period = std::chrono::milliseconds(pollMs);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            // 从数据库读取总功率限制值和逆变器数量
            himActivePowerLimit_ = static_cast<uint16_t>(db.select(kDataTotalPowerLimitAddr, "data_total"));
            int n = db.select(engine_->profile().count_qt_addr, "qt");
            if (n < 0) {
                n = 0;
            }
            if (static_cast<std::size_t>(n) > kMaxInverters) {
                n = static_cast<int>(kMaxInverters);
            }
            numberOfModules_ = static_cast<uint16_t>(n);
            // 逐台逆变器轮询
            for (int i = 1; i <= n; ++i) {
                pollOneInverter(db, i);
                if (i < n) {
                    sleepFrameGap();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[sun_pv] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[sun_pv] unknown error" << std::endl;
        }
        // 定时控制：确保每轮周期固定为 pollMs
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}