#ifndef AGC_H
#define AGC_H

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"

struct STR_AGC_Data {
    uint16_t ABVoltage = 0;  // 线电压AB
    uint16_t BCVoltage = 0;  // 线电压BC
    uint16_t CAVoltage = 0;  // 线电压CA
    uint16_t APhaseVoltage = 0;  // A相电压
    uint16_t BPhaseVoltage = 0;  // B相电压
    uint16_t CPhaseVoltage = 0;  // C相电压
    int16_t AFrequency = 0;  // A相频率
    int16_t BFrequency = 0;  // B相频率
    int16_t CFrequency = 0;  // C相频率
    int16_t APhaseCurrent = 0;  // A相电流
    int16_t BPhaseCurrent = 0;  // B相电流
    int16_t CPhaseCurrent = 0;  // C相电流
    int16_t APhaseActivePower = 0;  // A相有功功率
    int16_t BPhaseActivePower = 0;  // B相有功功率
    int16_t CPhaseActivePower = 0;  // C相有功功率
    int16_t TotalActivePower = 0;  // 总有功功率
    int16_t APhaseReactivePower = 0;  // A相无功功率
    int16_t BPhaseReactivePower = 0;  // B相无功功率
    int16_t CPhaseReactivePower = 0;  // C相无功功率
    int16_t TotalReactivePower = 0;  // 总无功功率
    int16_t APhaseApparentPower = 0;  // A相视在功率
    int16_t BPhaseApparentPower = 0;  // B相视在功率
    int16_t CPhaseApparentPower = 0;  // C相视在功率
    int16_t TotalApparentPower = 0;  // 总视在功率
    int32_t TotalActiveEnergy = 0;  // 总有功电能
    int32_t TotalReactiveEnergy = 0;  // 总无功电能
    int16_t TotalPowerFactor = 0;  // 总功率因数
    int16_t EngineOilLevel = 0; //发动机油位(0-100)
};

const double agcRatio_value2 = 0.01;
const double agcRatio_value1 = 0.1;

class Agc {
public:
    static constexpr std::size_t kAgcMaxGensets = 4;       // 柴发最大台数
    // qt 表 addr：柴发并机台数（与 SunPv 使用的 qt 等区分，按现场改）
    static constexpr int kQtAddrAgcGensetCount = 4;
    // com_alarm：电表 3～6；光伏 SunPv 12～15；柴发 16～18
    static constexpr int kComAlarmAgcCommAddrBase = 16;

    Agc(const char* serial_port, int baud_rate);
    ~Agc();
    void runAgcThread(MySQLConnectionPool& pool);

private:
    bool readAndWriteData(MySQLConnectionPool& pool, int slaveID, int amount);
    void agcHistory(int amount);

    ModbusRTU modbusclient;
    std::array<STR_AGC_Data, kAgcMaxGensets> agc_data{};    
    std::array<int, kAgcMaxGensets> commErrCount{};
    std::array<int, kAgcMaxGensets> commErrflg{};
    std::array<std::chrono::steady_clock::time_point, kAgcMaxGensets> lastHistoryTime{};
    uint16_t arr_uint16[50] = {0};
    std::string tableNameAGC = "agc";
    databaseList List;
    DatabaseList infList;
    influxDB influxDb;
};

#endif  // AGC_H
