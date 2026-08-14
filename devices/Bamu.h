#ifndef BAMU_H
#define BAMU_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <array>
#include <chrono>
#include <memory>
#include <string>

const double BamuRatio_Value3 = 0.001;

static constexpr int BMS_CELL_VOLTAGE_COUNT = 260;
static constexpr int BMS_CELL_TEMPERATURE_COUNT = 135;
static constexpr int BMS_CELL_MODBUS_BLOCK = 120;
static constexpr int BMS_CELL_MODBUS_BLOCK1 = 15;
static constexpr int BMS_CELL_MODBUS_BLOCK2 = 20;
static constexpr int BAMU_MAX_BMS_CLUSTER = 10;
static constexpr int BMS_CLUSTERS_PER_AIR_UNIT = 2;
static constexpr int BAMU_MAX_AIR_UNIT = (BAMU_MAX_BMS_CLUSTER + 1) / 2;
static constexpr int AIR_COM_ALARM_ADDR_BASE = 7;
static constexpr int DI_START_ADDR = 0x898;
static constexpr int DI_COUNT = 13;

inline bool isValidBmsClusterId(int id) { return id >= 1 && id <= BAMU_MAX_BMS_CLUSTER; }
inline int clampBmsClusterCount(int n)
{
    if (n < 1) return 1;
    if (n > BAMU_MAX_BMS_CLUSTER) return BAMU_MAX_BMS_CLUSTER;
    return n;
}
inline bool isValidAirUnitId(int id) { return id >= 1 && id <= BAMU_MAX_AIR_UNIT; }
inline int bmsClusterToAirUnitId(int id)
{
    return (id + BMS_CLUSTERS_PER_AIR_UNIT - 1) / BMS_CLUSTERS_PER_AIR_UNIT;
}
inline bool isAirUnitLeadCluster(int id)
{
    return (id - 1) % BMS_CLUSTERS_PER_AIR_UNIT == 0;
}
inline int airUnitToComAlarmAddr(int id) { return AIR_COM_ALARM_ADDR_BASE + id - 1; }
inline int airUnitToLogicCommAddr(int id) { return 201 + (id - 1) * 1000; }

struct BMSCellData {
    uint16_t Cell_Voltage[BMS_CELL_VOLTAGE_COUNT]{};
    uint16_t Cell_Temperature[BMS_CELL_TEMPERATURE_COUNT]{};
};

struct DiSignals {
    bool EPO = false;
    bool TotalSwitch = false;
    bool FireLevel1 = false;
    bool FireControl = false;
};

class Bamu {
public:
    explicit Bamu(const std::string& configPath = std::string());
    ~Bamu();
    void runBamuDataThread(MySQLConnectionPool& pool);

private:
    void onAfterStack(ModbusPollEngine& eng, MySQLDatabase& db);
    void onAfterBms(ModbusPollEngine& eng, MySQLDatabase& db);
    void onAfterAir(ModbusPollEngine& eng, MySQLDatabase& db);
    int computeAlarmLevel(const ModbusPollEngine& eng) const;
    bool readDiSignals(DiSignals& out);
    void writeLogicData(MySQLDatabase& db, const ModbusPollEngine& stack);
    void readBmsCellData(int id);
    void writeCellData(MySQLDatabase& db, int id);
    void processStack(MySQLDatabase& db);
    void processCluster(MySQLDatabase& db, int id);
    void flushDbList(MySQLDatabase& db, const std::string& table);

    std::unique_ptr<ModbusTCP> bus_;
    std::unique_ptr<ModbusPollEngine> stackEngine_;
    std::unique_ptr<ModbusPollEngine> bmsEngine_;
    std::unique_ptr<ModbusPollEngine> airEngine_;

    int batteryNumber_ = 1;
    int qtPollCounter_ = 0;
    DiSignals diSignals_{};
    databaseList dbList_;
    uint16_t arr_input_registers_[128]{};
    uint8_t arr_input_bits_[32]{};
    std::array<BMSCellData, BAMU_MAX_BMS_CLUSTER + 1> bmsCell_{};
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_BMS_CLUSTER + 1> lastCellRealtime_{};
    std::array<std::array<double, BMS_CELL_VOLTAGE_COUNT + 1>, BAMU_MAX_BMS_CLUSTER + 1> lastVolDb_{};
    std::array<std::array<double, BMS_CELL_TEMPERATURE_COUNT + 1>, BAMU_MAX_BMS_CLUSTER + 1> lastTempDb_{};
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasVolSnap_{};
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasTempSnap_{};
    std::array<double, BAMU_MAX_AIR_UNIT + 1> airOnline_{};
};

#endif
