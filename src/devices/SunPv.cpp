#include "SunPv.h"
#include "Config.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <thread>

namespace {

struct U16Range {
    uint16_t lo;
    uint16_t hi;
};

static bool inRange(uint16_t c, U16Range r) { return c >= r.lo && c <= r.hi; }

static bool inAnyRange(uint16_t c, const U16Range* tab, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (inRange(c, tab[i]))
            return true;
    }
    return false;
}

// 手册「系统故障」区间/离散合并为闭区间表（208 已单独归为光伏备用，此处不含 208）
static const U16Range kSystemFaultRanges[] = {
    {7, 7},       {11, 11},   {16, 16},   {19, 25},   {30, 34},   {36, 36},
    {38, 38},     {40, 42},   {44, 50},   {52, 58},   {60, 68},   {85, 85},
    {87, 87},     {92, 92},   {93, 93},   {100, 105}, {107, 114}, {116, 124},
    {200, 207},   {209, 211}, {248, 255}, {300, 322}, {324, 326}, {401, 412},
    {600, 603},   {605, 605}, {608, 608}, {612, 612}, {616, 616}, {620, 620},
    {622, 624},   {681, 681}, {800, 800}, {802, 802}, {804, 804}, {807, 807},
    {1096, 1122},
};

static const U16Range kSystemAlarmRanges[] = {
    {59, 59},     {70, 72},   {74, 74},   {76, 76},   {77, 81},   {82, 82},
    {83, 83},     {86, 86},   {89, 89},   {216, 218}, {220, 231}, {396, 397},
    {432, 434},   {500, 513}, {515, 518}, {635, 638}, {900, 900}, {901, 901},
    {910, 910},   {911, 911}, {1124, 1127},
};

} // namespace

void sunPvDecodeAlarm5044(uint16_t code5044, STR_SUNPV_Alarm5044& out)
{
    out = STR_SUNPV_Alarm5044{};
    if (code5044 == 0)
        return;

    if (code5044 == 2 || code5044 == 3 || code5044 == 14 || code5044 == 15) {
        out.GridOvervoltage = true;
        return;
    }
    if (code5044 == 4 || code5044 == 5) {
        out.GridUndervoltage = true;
        return;
    }
    if (code5044 == 8) {
        out.GridOverfrequency = true;
        return;
    }
    if (code5044 == 9) {
        out.GridUnderfrequency = true;
        return;
    }
    if (code5044 == 10) {
        out.GridPowerOutage = true;
        return;
    }
    if (code5044 == 12) {
        out.LeakageCurrentHigh = true;
        return;
    }
    if (code5044 == 13) {
        out.GridAbnormality = true;
        return;
    }
    if (code5044 == 17) {
        out.GridVoltageUnbalance = true;
        return;
    }
    if (code5044 == 28 || code5044 == 29 || code5044 == 208 || inRange(code5044, {448, 479})) {
        out.PvBackupConnectionFault = true;
        return;
    }
    if (inRange(code5044, {532, 547}) || inRange(code5044, {564, 579})) {
        out.PvReverseConnectionAlarm = true;
        return;
    }
    if (inRange(code5044, {548, 563}) || inRange(code5044, {580, 595})) {
        out.PvAbnormalityAlarm = true;
        return;
    }
    if (inRange(code5044, {264, 283})) {
        out.MpptReverseConnection = true;
        return;
    }
    if (inRange(code5044, {332, 363})) {
        out.BoostCapOvervoltageAlarm = true;
        return;
    }
    if (inRange(code5044, {364, 395})) {
        out.BoostCapOvervoltageFault = true;
        return;
    }
    if (code5044 == 1328) {
        out.PvCableShortToGround = true;
        return;
    }
    if (inRange(code5044, {1548, 1579})) {
        out.StringCurrentBackflow = true;
        return;
    }
    if (inRange(code5044, {1600, 1611})) {
        out.PvGroundingFault = true;
        return;
    }
    if (code5044 == 1616) {
        out.SystemHardwareFault = true;
        return;
    }
    if (code5044 == 37) {
        out.AmbientTempHigh = true;
        return;
    }
    if (code5044 == 43) {
        out.AmbientTempLow = true;
        return;
    }
    if (code5044 == 39) {
        out.InsulationResistanceLow = true;
        return;
    }
    if (code5044 == 106) {
        out.GroundCableFault = true;
        return;
    }
    if (code5044 == 88) {
        out.ArcFault = true;
        return;
    }
    if (code5044 == 84) {
        out.MeterCtReverseAlarm = true;
        return;
    }
    if (code5044 == 514) {
        out.MeterCommAbnormalityAlarm = true;
        return;
    }
    if (code5044 == 323) {
        out.GridConflict = true;
        return;
    }
    if (code5044 == 75) {
        out.InverterGridTiedCommAlarm = true;
        return;
    }

    if (inAnyRange(code5044, kSystemFaultRanges,
                   sizeof(kSystemFaultRanges) / sizeof(kSystemFaultRanges[0]))) {
        out.SystemFault = true;
        return;
    }
    if (inAnyRange(code5044, kSystemAlarmRanges,
                   sizeof(kSystemAlarmRanges) / sizeof(kSystemAlarmRanges[0]))) {
        out.SystemAlarm = true;
        return;
    }
}

// 5037 逆变器状态字 -> MPPTStatus：0 待机/启动 1 运行 2 停机 3 故障
static void applyMpptStatusFromInverterState(uint16_t st, uint16_t& mpptStatus)
{
    const auto eq = [st](SunPvInverterState state) {
        return st == static_cast<uint16_t>(state);
    };

    if (eq(SunPvInverterState::Running) || eq(SunPvInverterState::AlarmRunning)
        || eq(SunPvInverterState::DeratingRunning)
        || eq(SunPvInverterState::SchedulingRunning)) {
        mpptStatus = 1;
        return;
    }
    if (eq(SunPvInverterState::Stop) || eq(SunPvInverterState::ButtonStop)
        || eq(SunPvInverterState::EmergencyStop)) {
        mpptStatus = 2;
        return;
    }
    if (eq(SunPvInverterState::Standby) || eq(SunPvInverterState::InitialStandby)
        || eq(SunPvInverterState::Startup) || eq(SunPvInverterState::Uninitialized)) {
        mpptStatus = 0;
        return;
    }
    if (eq(SunPvInverterState::Fault) || eq(SunPvInverterState::CommunicationFault)) {
        mpptStatus = 3;
    }
}

namespace {

struct SunPvFieldContext {
    const STR_SUNPV_Data& data;
    const STR_SUNPV_Alarm5044& alarm5044;
    const STR_SUNPV_Alarm5151& alarm5151;
    int commFlag;
};

struct SunPvFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const SunPvFieldContext&);
};

#define SUNPV_FIELD_COMM(addr, key) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return static_cast<double>(c.commFlag); } }
#define SUNPV_FIELD_RAW(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return static_cast<double>(c.data.field); } }
#define SUNPV_FIELD_R1(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return static_cast<double>(c.data.field) * sunPvRatio_value1; } }
#define SUNPV_FIELD_R3(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return static_cast<double>(c.data.field) * sunPvRatio_value3; } }
#define SUNPV_FIELD_U32(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return static_cast<double>(c.data.field); } }
#define SUNPV_ALARM5044(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return c.alarm5044.field ? 1.0 : 0.0; } }
#define SUNPV_ALARM5151(addr, key, field) \
    { addr, key, [](const SunPvFieldContext& c) -> double { return c.alarm5151.field ? 1.0 : 0.0; } }

static const SunPvFieldMapEntry kSunPvFieldMaps[] = {
#include "SunPv_field_maps.inc"
};

static constexpr size_t kSunPvFieldMapCount = sizeof(kSunPvFieldMaps) / sizeof(SunPvFieldMapEntry);
static_assert(kSunPvFieldMapCount == 75, "SunPv field map count mismatch");

} // namespace

SunPv::SunPv(const char* serial_port, int baud_rate)
    : modbusclient(serial_port, baud_rate)
{
}

SunPv::~SunPv()
{
    modbusclient.disconnect();
}

bool SunPv::readAndWriteData(MySQLConnectionPool& pool, int slaveID, int amount)
{
    const int invIdx = amount - 1; // amount 与从站号一致 1..kSunPvMaxInverters
    if (invIdx < 0 || static_cast<std::size_t>(invIdx) >= kSunPvMaxInverters)
        return false;

    MySQLDatabase db(pool);
    const int comAlarmAddr = kComAlarmSunPvCommAddrBase + amount - 1;
    const std::string tableName = tableNameSunPv + std::to_string(amount);

    // 通信/读取失败处理：累计失败次数，超过阈值置告警标志，本次不写库
    auto failCycle = [&]() -> bool {
        commErrCount[invIdx]++;
        if (commErrCount[invIdx] > 3) {
            commErrflg[invIdx] = 1;
        }
        db.update(0, commErrflg[invIdx], tableName);
        db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");
        return false;
    };

    const bool commOk = modbusclient.connect(slaveID)
        && modbusclient.commTest(4999, 1, arr_uint16, tableName) != -1;

    if (!commOk) {
        return failCycle();
    }

    bool readOk = true;

    if (modbusclient.readInputRegisters(5002, 34, arr_uint16) != -1) {
        sunPv_data[invIdx].DailyGeneration = arr_uint16[0];
        sunPv_data[invIdx].TotalGeneration = (static_cast<uint32_t>(arr_uint16[2]) << 16) | arr_uint16[1];
        sunPv_data[invIdx].RunningTime = (static_cast<uint32_t>(arr_uint16[3]) << 16) | arr_uint16[4];
        sunPv_data[invIdx].AmbientTemperature = static_cast<int16_t>(arr_uint16[5]);
        sunPv_data[invIdx].TotalApparentPower = (static_cast<uint32_t>(arr_uint16[7]) << 16) | arr_uint16[6];
        sunPv_data[invIdx].Mppt1Voltage = arr_uint16[8];
        sunPv_data[invIdx].Mppt1Current = static_cast<int16_t>(arr_uint16[9]);
        sunPv_data[invIdx].Mppt2Voltage = arr_uint16[10];
        sunPv_data[invIdx].Mppt2Current = static_cast<int16_t>(arr_uint16[11]);
        sunPv_data[invIdx].Mppt3Voltage = arr_uint16[12];
        sunPv_data[invIdx].Mppt3Current = static_cast<int16_t>(arr_uint16[13]);
        sunPv_data[invIdx].DCTotalPower = (static_cast<uint32_t>(arr_uint16[15]) << 16) | arr_uint16[14];
        sunPv_data[invIdx].APhaseVoltage = arr_uint16[16];
        sunPv_data[invIdx].BPhaseVoltage = arr_uint16[17];
        sunPv_data[invIdx].CPhaseVoltage = arr_uint16[18];
        sunPv_data[invIdx].APhaseCurrent = static_cast<int16_t>(arr_uint16[19]);
        sunPv_data[invIdx].BPhaseCurrent = static_cast<int16_t>(arr_uint16[20]);
        sunPv_data[invIdx].CPhaseCurrent = static_cast<int16_t>(arr_uint16[21]);
        sunPv_data[invIdx].TotalActivePower = (static_cast<uint32_t>(arr_uint16[29]) << 16) | arr_uint16[28];
        sunPv_data[invIdx].TotalReactivePower = (static_cast<uint32_t>(arr_uint16[31]) << 16) | arr_uint16[30];
        sunPv_data[invIdx].TotalPowerFactor = static_cast<int16_t>(arr_uint16[32]);
        sunPv_data[invIdx].Frequency = arr_uint16[33];
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(5037, 1, arr_uint16) != -1) {
        applyMpptStatusFromInverterState(arr_uint16[0], sunPv_data[invIdx].MPPTStatus);
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(5044, 1, arr_uint16) != -1) {
        sunPvDecodeAlarm5044(arr_uint16[0], sunPv_alarm5044[invIdx]);
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(5114, 23, arr_uint16) != -1) {
        sunPv_data[invIdx].Mppt4Voltage = arr_uint16[0];
        sunPv_data[invIdx].Mppt4Current = static_cast<int16_t>(arr_uint16[1]);
        sunPv_data[invIdx].Mppt5Voltage = arr_uint16[2];
        sunPv_data[invIdx].Mppt5Current = static_cast<int16_t>(arr_uint16[3]);
        sunPv_data[invIdx].Mppt6Voltage = arr_uint16[4];
        sunPv_data[invIdx].Mppt6Current = static_cast<int16_t>(arr_uint16[5]);
        sunPv_data[invIdx].Mppt7Voltage = arr_uint16[6];
        sunPv_data[invIdx].Mppt7Current = static_cast<int16_t>(arr_uint16[7]);
        sunPv_data[invIdx].Mppt8Voltage = arr_uint16[8];
        sunPv_data[invIdx].Mppt8Current = static_cast<int16_t>(arr_uint16[9]);
        sunPv_data[invIdx].MonthlyGeneration = (static_cast<uint32_t>(arr_uint16[14]) << 16) | arr_uint16[13];
        sunPv_data[invIdx].Mppt9Voltage = arr_uint16[15];
        sunPv_data[invIdx].Mppt9Current = static_cast<int16_t>(arr_uint16[16]);
        sunPv_data[invIdx].Mppt10Voltage = arr_uint16[17];
        sunPv_data[invIdx].Mppt10Current = static_cast<int16_t>(arr_uint16[18]);
        sunPv_data[invIdx].Mppt11Voltage = arr_uint16[19];
        sunPv_data[invIdx].Mppt11Current = static_cast<int16_t>(arr_uint16[20]);
        sunPv_data[invIdx].Mppt12Voltage = arr_uint16[21];
        sunPv_data[invIdx].Mppt12Current = static_cast<int16_t>(arr_uint16[22]);
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(5150, 1, arr_uint16) != -1) {
        sunPv_alarm5151[invIdx] = STR_SUNPV_Alarm5151{};
        const uint16_t pid = arr_uint16[0];
        if (pid == 432) {
            sunPv_alarm5151[invIdx].PIDImpedanceAbnormal = true;
        } else if (pid == 433) {
            sunPv_alarm5151[invIdx].PIDFunctionAbnormal = true;
        } else if (pid == 434) {
            sunPv_alarm5151[invIdx].PIDVoltageCurrentProtection = true;
        }
    } else {
        readOk = false;
    }

    if (modbusclient.readRegisters(5006, 2, arr_uint16) != -1) {
        sunPv_setData[invIdx].ActivePowerLimitSwitch = arr_uint16[0];
        sunPv_setData[invIdx].ActivePowerLimit = arr_uint16[1];
    } else {
        readOk = false;
    }

    // 任一组寄存器读取失败：本轮不写库，避免把上一台逆变器残留数据写入本表
    if (!readOk) {
        return failCycle();
    }

    // 整轮读取成功：清除累计错误，恢复通信正常标志
    commErrCount[invIdx] = 0;
    commErrflg[invIdx] = 0;
    db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");

    if (sunPv_himSetData.NumberOfModules > 0 && sunPv_himSetData.RatedPower > 0) {
        // 5007 单位 0.1%：百分比 × 10，四舍五入（如 10.4% → 104）
        const uint16_t activePowerLimit = static_cast<uint16_t>(std::round(
            static_cast<double>(sunPv_himSetData.ActivePowerLimit)
            / sunPv_himSetData.NumberOfModules
            / sunPv_himSetData.RatedPower * 1000.0));
        constexpr int kPowerLimitWriteDeadband = 10;
        const uint16_t cur = static_cast<uint16_t>(sunPv_setData[invIdx].ActivePowerLimit);
        const uint16_t target = static_cast<uint16_t>(activePowerLimit);
        const uint16_t diff = target - cur;
        if (diff > kPowerLimitWriteDeadband || diff < -kPowerLimitWriteDeadband) {
            modbusclient.writeRegister(5007, activePowerLimit, "sunPv");
            std::cout << "SunPv有功功率限制指令下发:" << activePowerLimit << std::endl;
            LOG_ACTION("SunPv有功功率限制指令下发:" + std::to_string(activePowerLimit));
        }
    }

    writeDataToDatabase(pool, amount);
    auto now = std::chrono::steady_clock::now();
    if (lastHistoryTime[invIdx] == std::chrono::steady_clock::time_point{}
        || now - lastHistoryTime[invIdx] >= std::chrono::seconds(30)) {
        writeDataHistory(amount);
        lastHistoryTime[invIdx] = now;
    }

    return true;
}

void SunPv::writeDataToDatabase(MySQLConnectionPool& pool, int amount){
    const int idx = amount - 1;
    if (idx < 0 || static_cast<std::size_t>(idx) >= kSunPvMaxInverters)
        return;
    MySQLDatabase db(pool);
    const SunPvFieldContext ctx{sunPv_data[idx], sunPv_alarm5044[idx], sunPv_alarm5151[idx], commErrflg[idx]};
    List.clearData();
    for (const auto& field : kSunPvFieldMaps) {
        List.addData(field.dbAddr, field.getter(ctx));
    }
    db.insert(List.spliceData(tableNameSunPv + std::to_string(amount)));
}

void SunPv::writeDataHistory(int amount){
    const int idx = amount - 1;
    if (idx < 0 || static_cast<std::size_t>(idx) >= kSunPvMaxInverters)
        return;
    const SunPvFieldContext ctx{sunPv_data[idx], sunPv_alarm5044[idx], sunPv_alarm5151[idx], commErrflg[idx]};
    infList.clearData();
    for (const auto& field : kSunPvFieldMaps) {
        infList.addData(field.influxKey, field.getter(ctx));
    }
    influxDb.insert(infList.spliceData(tableNameSunPv + std::to_string(amount)));
}

void SunPv::runSunPvThread(MySQLConnectionPool& pool){
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cyclePeriod = std::chrono::milliseconds(Config::SUN_PV_DATA_INTERVAL);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            sunPv_himSetData.ActivePowerLimit = static_cast<uint16_t>(db.select(110, "data_total"));
            const int rawModules = db.select(617, "qt");
            sunPv_himSetData.RatedPower = 125;

            int n = rawModules < 0 ? 0 : rawModules;
            if (static_cast<std::size_t>(n) > kSunPvMaxInverters)
                n = static_cast<int>(kSunPvMaxInverters);
            sunPv_himSetData.NumberOfModules = static_cast<uint16_t>(n);

            if (n > 0) {
                for (int i = 1; i <= n; ++i) {
                    readAndWriteData(pool, i, i);
                    if (i < n) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(Config::METER_SLAVE_SWITCH_DELAY_MS));
                    }
                }
            }

        } catch (const std::exception& e) {
            std::cerr << "SunPv线程错误: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "SunPv线程未知错误" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}
