#ifndef AMC_METER_H
#define AMC_METER_H

#include <array>
#include <cstdint>
#include <chrono>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"

struct STR_Mater_Data {
    uint16_t Ct = 0;
    uint16_t Pt = 0;
    uint16_t APhaseVoltage = 0;
    uint16_t BPhaseVoltage = 0;
    uint16_t CPhaseVoltage = 0;
    uint16_t ABVoltage = 0;
    uint16_t BCVoltage = 0;
    uint16_t CAVoltage = 0;
    int16_t APhaseCurrent = 0;
    int16_t BPhaseCurrent = 0;
    int16_t CPhaseCurrent = 0;
    int16_t NCurrent = 0;
    int32_t APhaseActivePower = 0;
    int32_t BPhaseActivePower = 0;
    int32_t CPhaseActivePower = 0;
    int32_t TotalActivePower = 0;
    int32_t APhaseReactivePower = 0;
    int32_t BPhaseReactivePower = 0;
    int32_t CPhaseReactivePower = 0;
    int32_t TotalReactivePower = 0;
    int32_t APhaseApparentPower = 0;
    int32_t BPhaseApparentPower = 0;
    int32_t CPhaseApparentPower = 0;
    int32_t TotalApparentPower = 0;
    int16_t APowerFactor = 0;
    int16_t BPowerFactor = 0;
    int16_t CPowerFactor = 0;
    int16_t TotalPowerFactor = 0;
    uint16_t Frequency = 0;
    uint32_t tEP = 0;
    uint32_t imptEP = 0;
    uint32_t exptEP = 0;
    uint32_t tEQ = 0;
    uint32_t imptEQ = 0;
    uint32_t exptEQ = 0;

    // 003FH~0042H 有功电能（二次侧与一次侧）
    uint32_t absorbActiveEnergySecondary = 0; // 003FH~0040H 吸收有功电能二次侧
    uint32_t releaseActiveEnergySecondary = 0; // 0041H~0042H 释放有功电能二次侧
    double absorbActiveEnergyPrimary = 0.0;   // 一次侧吸收有功电能(kWh)
    double releaseActiveEnergyPrimary = 0.0;  // 一次侧释放有功电能(kWh)

    // 0023H/0024H 小数点和符号
    uint8_t voltageDecimalPointDPT = 0; // 0023H高字节
    uint8_t currentDecimalPointDCT = 0; // 0023H低字节
    uint8_t powerDecimalPointDPQ = 0;   // 0024H高字节
    uint8_t pqSign = 0;                 // 0024H低字节

    // pqSign 位定义（高位->低位：Q、Qc、Qb、Qa、P、Pc、Pb、Pa；0正1负）
    bool qNegative = false;
    bool qcNegative = false;
    bool qbNegative = false;
    bool qaNegative = false;
    bool pNegative = false;
    bool pcNegative = false;
    bool pbNegative = false;
    bool paNegative = false;
};

const double meterRatio_value1 = 0.1;
const double meterRatio_value2 = 0.01;
const double meterRatio_value3 = 0.001;

class AMC_Meter {
public:
    // com_alarm：amount 1~4 对应 addr 3~6（负载、柴发、光伏、储能电表）
    static constexpr int kComAlarmMeterCommAddrBase = 3;
    static constexpr std::size_t kMeterLogicalSlotCount = 4; // amount 取值 1~4

    AMC_Meter(const char* serial_port, int baud_rate);
    ~AMC_Meter();

    void runLoadDgThread(MySQLConnectionPool& pool);
    void runPvEssThread(MySQLConnectionPool& pool);

private:
    bool readAndWriteData(MySQLConnectionPool &pool, int slaveID, int amount);
    void meterHistory(int amount);

    ModbusRTU modbusclient;
    uint16_t arr_uint16[60] = {0};
    influxDB meterDB;
    DatabaseList infList;
    databaseList List;
    std::array<STR_Mater_Data, kMeterLogicalSlotCount> meter_data{};
    std::array<int, kMeterLogicalSlotCount> commErrCount{};
    std::array<int, kMeterLogicalSlotCount> commErrflg{};
    std::array<std::chrono::steady_clock::time_point, kMeterLogicalSlotCount> lastHistoryTime{};
    std::string tableNameMeter = "meter";
    std::string tableNameLoadMeter = "load_meter";
    std::string tableNamePvMeter = "pv_meter";
    std::string tableNameDgMeter = "dg_meter";
    std::string tableNameEssMeter = "ess_meter";
};

#endif