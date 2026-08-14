#ifndef AMC_METER_H
#define AMC_METER_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

/**
 * 同总线双电表：load+dg 或 pv+ess。
 * 连接/点表来自 config/modbus/devices.json + templates/amc_meter.json。
 */
class AMC_Meter {
public:
    AMC_Meter(const std::string& deviceIdA, const std::string& deviceIdB,
              const std::string& configPath = std::string());
    ~AMC_Meter();

    void runPairThread(MySQLConnectionPool& pool);

private:
    void attachLogicHook(ModbusPollEngine& eng);
    void writeLogic(ModbusPollEngine& eng, MySQLDatabase& db);

    std::unique_ptr<ModbusRTU> bus_;
    std::unique_ptr<ModbusPollEngine> engA_;
    std::unique_ptr<ModbusPollEngine> engB_;
};

#endif
