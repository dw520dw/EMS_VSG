#ifndef SUNPV_H
#define SUNPV_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <array>
#include <memory>
#include <string>

class SunPv {
public:
    static constexpr std::size_t kMaxInverters = 8;
    static constexpr int kComAlarmAddrBase = 12;
    static constexpr int kDataTotalPowerLimitAddr = 110;
    static constexpr uint16_t kRatedPowerKw = 125;

    explicit SunPv(const std::string& configPath = std::string());
    ~SunPv();
    void runSunPvThread(MySQLConnectionPool& pool);

private:
    void onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db);
    void pollOneInverter(MySQLDatabase& db, int amount);
    void writePowerLimit(ModbusPollEngine& eng);
    void sleepFrameGap() const;

    std::unique_ptr<ModbusRTU> bus_;
    std::unique_ptr<ModbusPollEngine> engine_;
    std::array<int, kMaxInverters> commErrCount_{};
    uint16_t himActivePowerLimit_ = 0;
    uint16_t numberOfModules_ = 0;
    uint16_t probeBuf_ = 0;
};

#endif
