#ifndef PCS_SMARTEN_H
#define PCS_SMARTEN_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <cstdint>
#include <memory>
#include <string>

struct SmartenSetData {
    uint16_t alarmReset_41378 = 0;
    uint16_t pcsOnOff_41379 = 0;
    int16_t activePowerSetting_41546 = 0;
    int16_t reactivePowerSetting_41547 = 0;
    uint16_t gridInterMode_41671 = 0;
    uint16_t dcVoltageLowerLimit_41473 = 0;
    uint16_t constantVoltageChargeVoltage_41474 = 0;
    uint16_t dcOutputVoltage_41475 = 0;
    uint16_t dischargeTerminationVoltage_41478 = 0;
    uint16_t chargeCutoffCurrent_41480 = 0;
    uint16_t batteryProtectionSoc_41489 = 0;
    uint16_t dischargeLimitSoc_41490 = 0;
    uint16_t chargeLimitSoc_41491 = 0;
    uint16_t vsgeEnable_41682 = 0;
    uint16_t vsgeControlMode_41687 = 0;
    uint16_t acConnectType_41580 = 0;
    uint16_t threePhaseUnbalancedMode_41582 = 0;
    uint16_t ratedVoltageLevel_41543 = 0;
    uint16_t ratedFrequencyLevel_41544 = 0;
    uint16_t energyControlMode_41701 = 0;
    bool antiBackflowProtection_41696_bit1 = false;
    bool powerFactorControl_41696_bit2 = false;
    bool threePhaseUnbalancedMode_41696_bit3 = false;
};

class PCS_Smarten {
public:
    explicit PCS_Smarten(const std::string& configPath = std::string());
    ~PCS_Smarten();
    void runPcsThread(MySQLConnectionPool& pool);

private:
    void onAfterTelemetry(ModbusPollEngine& eng, MySQLDatabase& db);
    void fillVirtual(ModbusPollEngine& eng);
    void dispatchHmiControlCommands(MySQLDatabase& db);
    void pcsSetData(MySQLDatabase& db);
    void writeLogicData(ModbusPollEngine& eng, MySQLDatabase& db);
    double readU64Scaled(int startAddr, double scale);

    std::unique_ptr<ModbusTCP> bus_;
    std::unique_ptr<ModbusPollEngine> engine_;
    SmartenSetData deviceSet_{};
    SmartenSetData hmiSet_{};
    databaseList listPcs_;
    uint16_t arr_[32]{};
    int pcsRead_ = 0;
    int pcsWrite_ = 0;
};

#endif
