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

enum class SunPvInverterState : uint16_t {
    Running = 0x0000,
    Stop = 0x8000,
    ButtonStop = 0x1300,
    EmergencyStop = 0x1500,
    Standby = 0x1400,
    InitialStandby = 0x1200,
    Startup = 0x1600,
    AlarmRunning = 0x9100,
    DeratingRunning = 0x8100,
    SchedulingRunning = 0x8200,
    Fault = 0x5500,
    CommunicationFault = 0x2500,
    Uninitialized = 0x1111,
};

struct U16Range {
    uint16_t lo;
    uint16_t hi;
};

struct Alarm5044 {
    bool GridOvervoltage = false;
    bool GridUndervoltage = false;
    bool GridOverfrequency = false;
    bool GridUnderfrequency = false;
    bool GridPowerOutage = false;
    bool LeakageCurrentHigh = false;
    bool GridAbnormality = false;
    bool GridVoltageUnbalance = false;
    bool PvBackupConnectionFault = false;
    bool PvReverseConnectionAlarm = false;
    bool PvAbnormalityAlarm = false;
    bool MpptReverseConnection = false;
    bool BoostCapOvervoltageAlarm = false;
    bool BoostCapOvervoltageFault = false;
    bool PvCableShortToGround = false;
    bool StringCurrentBackflow = false;
    bool PvGroundingFault = false;
    bool SystemHardwareFault = false;
    bool AmbientTempHigh = false;
    bool AmbientTempLow = false;
    bool InsulationResistanceLow = false;
    bool GroundCableFault = false;
    bool ArcFault = false;
    bool MeterCtReverseAlarm = false;
    bool MeterCommAbnormalityAlarm = false;
    bool GridConflict = false;
    bool InverterGridTiedCommAlarm = false;
    bool SystemFault = false;
    bool SystemAlarm = false;
};

static bool inRange(uint16_t c, U16Range r) { return c >= r.lo && c <= r.hi; }

static bool inAnyRange(uint16_t c, const U16Range* tab, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) {
        if (inRange(c, tab[i])) {
            return true;
        }
    }
    return false;
}

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

static void decodeAlarm5044(uint16_t code, Alarm5044& out)
{
    out = Alarm5044{};
    if (code == 0) {
        return;
    }
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
    if (inAnyRange(code, kSystemFaultRanges, sizeof(kSystemFaultRanges) / sizeof(kSystemFaultRanges[0]))) {
        out.SystemFault = true;
        return;
    }
    if (inAnyRange(code, kSystemAlarmRanges, sizeof(kSystemAlarmRanges) / sizeof(kSystemAlarmRanges[0]))) {
        out.SystemAlarm = true;
    }
}

static uint16_t mpptStatusFromState(uint16_t st)
{
    const auto eq = [st](SunPvInverterState s) { return st == static_cast<uint16_t>(s); };
    if (eq(SunPvInverterState::Running) || eq(SunPvInverterState::AlarmRunning)
        || eq(SunPvInverterState::DeratingRunning) || eq(SunPvInverterState::SchedulingRunning)) {
        return 1;
    }
    if (eq(SunPvInverterState::Stop) || eq(SunPvInverterState::ButtonStop)
        || eq(SunPvInverterState::EmergencyStop)) {
        return 2;
    }
    if (eq(SunPvInverterState::Standby) || eq(SunPvInverterState::InitialStandby)
        || eq(SunPvInverterState::Startup) || eq(SunPvInverterState::Uninitialized)) {
        return 0;
    }
    if (eq(SunPvInverterState::Fault) || eq(SunPvInverterState::CommunicationFault)) {
        return 3;
    }
    return 0;
}

}  // namespace

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
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterDecode(eng, static_cast<MySQLDatabase&>(sink));
    });
    std::cout << "[sun_pv] rtu=" << engine_->profile().rtu_device << std::endl;
}

SunPv::~SunPv()
{
    if (bus_) {
        bus_->disconnect();
    }
}

void SunPv::sleepFrameGap() const
{
    const int ms = engine_->profile().inter_frame_ms;
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

void SunPv::writePowerLimit(ModbusPollEngine& eng)
{
    if (numberOfModules_ == 0 || kRatedPowerKw == 0) {
        return;
    }
    const uint16_t target = static_cast<uint16_t>(std::round(
        static_cast<double>(himActivePowerLimit_) / numberOfModules_ / kRatedPowerKw * 1000.0));
    const int cur = static_cast<int>(eng.getValue("_ActivePowerLimit"));
    const int diff = static_cast<int>(target) - cur;
    constexpr int kDeadband = 10;
    if (diff <= kDeadband && diff >= -kDeadband) {
        return;
    }
    bus_->writeRegister(5007, target, eng.profile().resolvedMysqlTable());
    std::cout << "SunPv有功功率限制指令下发:" << target << std::endl;
    LOG_ACTION("SunPv有功功率限制指令下发:" + std::to_string(target));
}

void SunPv::onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db)
{
    (void)db;
    if (eng.commFlag() != 0) {
        return;
    }
    eng.setValue("MPPTStatus", static_cast<double>(
        mpptStatusFromState(static_cast<uint16_t>(eng.getValue("_InverterState")))));

    Alarm5044 a;
    decodeAlarm5044(static_cast<uint16_t>(eng.getValue("_Alarm5044")), a);
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

    const uint16_t pid = static_cast<uint16_t>(eng.getValue("_PidCode"));
    setb("PIDImpedanceAbnormal", pid == 432);
    setb("PIDFunctionAbnormal", pid == 433);
    setb("PIDVoltageCurrentProtection", pid == 434);

    writePowerLimit(eng);
}

void SunPv::pollOneInverter(MySQLDatabase& db, int amount)
{
    const int idx = amount - 1;
    engine_->profile().slave = amount;
    engine_->setTableSuffix(amount);
    engine_->profile().comm.com_alarm_addr = kComAlarmAddrBase + idx;
    engine_->clearCaches();
    bus_->setSlave(amount);

    const std::string table = engine_->profile().resolvedMysqlTable();
    const bool ok = bus_->probe(engine_->profile().comm.probe_addr,
                                engine_->profile().comm.probe_count, &probeBuf_, table);
    sleepFrameGap();
    if (!ok) {
        ++commErrCount_[static_cast<std::size_t>(idx)];
        const int offline = commErrCount_[static_cast<std::size_t>(idx)] > 3 ? 1 : 0;
        db.update(0, offline, table);
        db.update(engine_->profile().comm.com_alarm_addr, offline, "com_alarm");
        return;
    }

    commErrCount_[static_cast<std::size_t>(idx)] = 0;
    engine_->setInheritCommFlag(0);
    engine_->pollOnce(db);
}

void SunPv::runSunPvThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 600;
    const auto period = std::chrono::milliseconds(pollMs);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            himActivePowerLimit_ = static_cast<uint16_t>(db.select(kDataTotalPowerLimitAddr, "data_total"));
            int n = db.select(engine_->profile().count_qt_addr, "qt");
            if (n < 0) {
                n = 0;
            }
            if (static_cast<std::size_t>(n) > kMaxInverters) {
                n = static_cast<int>(kMaxInverters);
            }
            numberOfModules_ = static_cast<uint16_t>(n);
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
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}
