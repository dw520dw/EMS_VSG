#ifndef PCS_SMARTEN_H
#define PCS_SMARTEN_H

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"

enum class PCS_SmartenRunState : uint16_t {
    Stop = 0,                 // 停止
    Running = 1,              // 运行
    Alarm = 2,               // 报警
    Fault = 3,                // 故障
};

enum class PCS_SmartenOnOffGridState : uint16_t {
    GridOn = 0,                   // 并网
    GridOff = 1,                 // 离网
    Unknown = 2,                 // 未知（并网/离网位均无效）
};

struct SmartenData {

    // ---------- 40164~40167 交流母线汇总 ----------
    int16_t acBusTotalActivePower_40164 = 0;    // 40164 总有功
    int16_t acBusTotalApparentPower_40165 = 0;  // 40165 总视在
    int16_t acBusTotalReactivePower_40166 = 0;  // 40166 总无功
    int16_t acBusTotalPowerFactor_40167 = 0;    // 40167 总功率因数

    // ---------- 40171 ----------
    uint32_t acBusFrequency_40171 = 0;          // 40171 uint32 交流母线频率（2 寄存器）

    // ---------- 40173 / 40177 交流口电量 uint64 ----------
    uint64_t acBusDischargedEnergy_40173 = 0;   // 40173 交流放电电量
    uint64_t acBusChargedEnergy_40177 = 0;      // 40177 交流充电电量

    // ---------- 40189~40201 环境 + 交流母线 L1 ----------
    int16_t ambientTemperature_40189 = 0;       // 40189 环境温度
    int16_t cabinetTemperature_40190 = 0;       // 40190 机柜温度
    int16_t moduleTemperature_40193 = 0;      // 40193 模块/IGBT 温度
    int16_t acBusL1ActivePower_40195 = 0;       // 40195 L1 有功
    int16_t acBusL1ApparentPower_40196 = 0;     // 40196 L1 视在
    int16_t acBusL1ReactivePower_40197 = 0;     // 40197 L1 无功
    int16_t acBusL1PowerFactor_40198 = 0;       // 40198 L1 功率因数
    int16_t acBusL1Current_40199 = 0;           // 40199 L1 电流
    int16_t acBusL1L2LineVoltage_40200 = 0;     // 40200 L1-L2 线电压(AB)
    int16_t acBusL1NVoltage_40201 = 0;          // 40201 L1-N 相电压(AN)

    // ---------- 40218~40224 交流母线 L2 ----------
    int16_t acBusL2ActivePower_40218 = 0;      // 40218 L2 有功
    int16_t acBusL2ApparentPower_40219 = 0;    // 40219 L2 视在
    int16_t acBusL2ReactivePower_40220 = 0;    // 40220 L2 无功
    int16_t acBusL2PowerFactor_40221 = 0;      // 40221 L2 功率因数
    int16_t acBusL2Current_40222 = 0;          // 40222 L2 电流
    int16_t acBusL2L3LineVoltage_40223 = 0;     // 40223 L2-L3(BC)
    int16_t acBusL2NVoltage_40224 = 0;          // 40224 L2-N(BN)

    // ---------- 40241~40247 交流母线 L3 ----------
    int16_t acBusL3ActivePower_40241 = 0;      // 40241 L3 有功
    int16_t acBusL3ApparentPower_40242 = 0;    // 40242 L3 视在
    int16_t acBusL3ReactivePower_40243 = 0;    // 40243 L3 无功
    int16_t acBusL3PowerFactor_40244 = 0;      // 40244 L3 功率因数
    int16_t acBusL3Current_40245 = 0;          // 40245 L3 电流
    int16_t acBusL3L1LineVoltage_40246 = 0;     // L3-L1(CA)
    int16_t acBusL3NVoltage_40247 = 0;          // L3-N(CN)

    // ---------- 41842~41849 直流侧 ----------
    int16_t dcPower_41842 = 0;                  // 41842 直流功率
    int16_t dcCurrent_41843 = 0;                // 41843 直流电流
    int16_t dcInputVoltage_41844 = 0;            // 41844 直流输入电压
    uint64_t dcDischargeEnergy_41845 = 0;       // 41845 直流放电电量 uint64
    uint64_t dcChargeEnergy_41849 = 0;        // 41849 直流充电电量 uint64

    // ---------- 41885~41922 电网侧（仅保留当前项目使用点位）----------
    int16_t gridTotalPowerFactor_41885 = 0; // 41885 电网总功率因数
    int16_t gridTotalActivePower_41886 = 0; // 41886 电网总有功
    int16_t gridTotalReactivePower_41887 = 0; // 41887 电网总无功
    int16_t gridTotalApparentPower_41888 = 0; // 41888 电网总视在
    int16_t gridFrequency_41889 = 0; // 41889 电网频率
    int16_t gridL1PowerFactor_41893 = 0; // 41893 L1 功率因数
    int16_t gridL1ActivePower_41894 = 0; // 41894 L1 有功
    int16_t gridL1ReactivePower_41895 = 0; // 41895 L1 无功
    int16_t gridL1ApparentPower_41896 = 0; // 41896 L1 视在
    int16_t gridL1Current_41897 = 0; // 41897 L1 电流
    int16_t gridL2PowerFactor_41901 = 0; // 41901 L2 功率因数
    int16_t gridL2ActivePower_41902 = 0; // 41902 L2 有功
    int16_t gridL2ReactivePower_41903 = 0; // 41903 L2 无功
    int16_t gridL2ApparentPower_41904 = 0; // 41904 L2 视在
    int16_t gridL2Current_41905 = 0; // 41905 L2 电流
    int16_t gridL3PowerFactor_41909 = 0; // 41909 L3 功率因数
    int16_t gridL3ActivePower_41910 = 0; // 41910 L3 有功
    int16_t gridL3ReactivePower_41911 = 0; // 41911 L3 无功
    int16_t gridL3ApparentPower_41912 = 0; // 41912 L3 视在
    int16_t gridL3Current_41913 = 0; // 41913 L3 电流
    int16_t gridL1L2LineVoltage_41917 = 0; // 41917 L1-L2 线电压(AB)
    int16_t gridL2L3LineVoltage_41918 = 0; // 41918 L2-L3 线电压(BC)
    int16_t gridL3L1LineVoltage_41919 = 0; // 41919 L3-L1 线电压(CA)
    int16_t gridL1NVoltage_41920 = 0; // 41920 L1-N 相电压(AN)
    int16_t gridL2NVoltage_41921 = 0; // 41921 L2-N 相电压(BN)
    int16_t gridL3NVoltage_41922 = 0; // 41922 L3-N 相电压(CN)

    // ---------- 41946~41971 负载侧（仅保留当前项目使用点位）----------
    int16_t loadTotalPowerFactor_41946 = 0; // 41946 总功率因数
    int16_t loadTotalActivePower_41947 = 0; // 41947 总有功
    int16_t loadTotalReactivePower_41948 = 0; // 41948 总无功
    int16_t loadTotalApparentPower_41949 = 0; // 41949 总视在
    int16_t loadFrequency_41950 = 0; // 41950 频率
    int16_t loadL1PowerFactor_41951 = 0; // 41951 L1 功率因数
    int16_t loadL1ActivePower_41952 = 0; // 41952 L1 有功
    int16_t loadL1ReactivePower_41953 = 0; // 41953 L1 无功
    int16_t loadL1ApparentPower_41954 = 0; // 41954 L1 视在
    int16_t loadL1Current_41955 = 0; // 41955 L1 电流
    int16_t loadL2PowerFactor_41956 = 0; // 41956 L2 功率因数
    int16_t loadL2ActivePower_41957 = 0; // 41957 L2 有功
    int16_t loadL2ReactivePower_41958 = 0; // 41958 L2 无功
    int16_t loadL2ApparentPower_41959 = 0; // 41959 L2 视在
    int16_t loadL2Current_41960 = 0; // 41960 L2 电流
    int16_t loadL3PowerFactor_41961 = 0; // 41961 L3 功率因数
    int16_t loadL3ActivePower_41962 = 0; // 41962 L3 有功
    int16_t loadL3ReactivePower_41963 = 0; // 41963 L3 无功
    int16_t loadL3ApparentPower_41964 = 0; // 41964 L3 视在
    int16_t loadL3Current_41965 = 0; // 41965 L3 电流
    int16_t loadL1L2LineVoltage_41966 = 0; // 41966 L1-L2 线电压(AB)
    int16_t loadL2L3LineVoltage_41967 = 0; // 41967 L2-L3 线电压(BC)
    int16_t loadL3L1LineVoltage_41968 = 0; // 41968 L3-L1 线电压(CA)
    int16_t loadL1NVoltage_41969 = 0; // 41969 L1-N 相电压(AN)
    int16_t loadL2NVoltage_41970 = 0; // 41970 L2-N 相电压(BN)
    int16_t loadL3NVoltage_41971 = 0; // 41971 L3-N 相电压(CN)

    // 41725~41728 状态量
    uint16_t runState = 0; // 运行状态
    uint16_t gridState = 0; // 电网状态
    bool faultState_41725_bit0 = false; // 41725 故障状态
    bool alarmState_41725_bit1 = false; // 41725 告警状态
    bool totalFanFault_41725_bit2 = false; // 41725 总风扇故障
    bool totalAuxPowerFault_41725_bit3 = false; // 41725 总辅助电源故障
    bool totalOverTemperatureFault_41725_bit4 = false; // 41725 总过温故障
    bool totalModuleOverCurrentFault_41725_bit5 = false; // 41725 总模块过流故障
    bool totalModuleCurrentLimitAlarm_41725_bit6 = false; // 41725 总模块电流限制告警
    bool ambientOverTemperatureFault_41725_bit7 = false; // 41725 环境过温故障
    bool moduleCurrentUnbalanceFault_41725_bit8 = false; // 41725 模块电流不平衡故障

    bool startStopState_41726_bit1 = false; // 41726 启动停止状态
    bool startingState_41726_bit2 = false; // 41726 启动状态
    bool standbyState_41726_bit3 = false; // 41726 待机状态
    bool onGridState_41726_bit4 = false; // 41726 并网状态
    bool offGridState_41726_bit5 = false; // 41726 离网状态
    bool chargeState_41726_bit6 = false; // 41726 充电状态
    bool dischargeState_41726_bit7 = false; // 41726 放电状态
    bool deratingState_41726_bit8 = false; // 41726 降容状态
    bool fullChargeState_41726_bit9 = false; // 41726 满充状态
    bool emptyDischargeState_41726_bit10 = false; // 41726 空放状态
    bool equalizationChargeState_41726_bit11 = false; // 41726 均衡充电状态
    bool floatChargeState_41726_bit12 = false; // 41726 浮充状态

    bool droopState_41727_bit1 = false; // 41727 下垂状态
    bool synchronizationState_41727_bit2 = false; // 41727 同步状态
    bool synchronizationDoneState_41727_bit3 = false; // 41727 同步完成状态

    bool emergencyShutdown_41728_bit0 = false; // 41728 紧急关机

    // ---------- 41729 软硬件初始化/版本类告警 ----------
    bool dspInitializingAbnormal_41729_bit0 = false; // 41729 DSP初始化异常
    bool dspVersionAbnormal_41729_bit1 = false; // 41729 DSP版本异常
    bool dspParameterMismatch_41729_bit2 = false; // 41729 DSP参数不匹配
    bool cpldVersionAbnormal_41729_bit3 = false; // 41729 CPLD版本异常
    bool flashAbnormal_41729_bit4 = false; // 41729 闪存异常

    // ---------- 41730 ----------
    bool monitorInitializingAbnormal_41730_bit0 = false; // 41730 监控初始化异常
    bool monitorParameterMismatch_41730_bit1 = false; // 41730 监控参数不匹配

    // ---------- 41731 ----------
    bool hardwareVersionError_41731_bit0 = false; // 41731 硬件版本异常
    bool idReduplicatedFault_41731_bit1 = false; // 41731 ID重复故障

    // ---------- 41732 ----------
    bool calibrationParameterError_41732_bit0 = false; // 41732 校准参数异常
    bool samplingZeroPointError_41732_bit1 = false; // 41732 采样零点异常

    // ---------- 41733 通讯类告警 ----------
    bool canACommunicationFault_41733_bit0 = false; // 41733 CAN A通讯异常
    bool canBCommunicationFault_41733_bit1 = false; // 41733 CAN B通讯异常
    bool canCCommunicationFault_41733_bit2 = false; // 41733 CAN C通讯异常
    bool canDCommunicationFault_41733_bit3 = false; // 41733 CAN D通讯异常
    bool rs485_1CommunicationFault_41733_bit4 = false; // 41733 RS485_1通讯异常
    bool rs485_2CommunicationFault_41733_bit5 = false; // 41733 RS485_2通讯异常
    bool rs485_3CommunicationFault_41733_bit6 = false; // 41733 RS485_3通讯异常
    bool rs485_4CommunicationFault_41733_bit7 = false; // 41733 RS485_4通讯异常
    bool auxiliaryBoardCommunicationFault_41733_bit8 = false; // 41733 辅助板通讯异常
    bool spiCommunicationFault_41733_bit9 = false; // 41733 SPI通讯异常
    bool emsCommConnectionTimeout_41733_bit10 = false; // 41733 EMS通讯超时
    bool rs485CommunicationFault_41733_bit11 = false; // 41733 RS485通讯异常

    // ---------- 41734 ----------
    bool synchronizationSignal1Fault_41734_bit0 = false; // 41734 同步信号1异常
    bool synchronizationSignal2Fault_41734_bit1 = false; // 41734 同步信号2异常

    // ---------- 41746 降额 ----------
    bool totalDeratingAbnormal_41746_bit0 = false;   // 总降额异常
    bool moduleOvertempDerating_41746_bit1 = false; // 模块过温降额
    bool cabinetOvertempDerating_41746_bit2 = false; // 机柜过温降额
    bool ambientOvertempDerating_41746_bit3 = false; // 环境过温降额

    // ---------- 41751 直流输入 ----------
    bool dcInputOvervoltage_41751_bit0 = false; // 41751 直流输入过压
    bool dcInputUndervoltage_41751_bit1 = false; // 41751 直流输入欠压
    bool dcInputReverse_41751_bit2 = false; // 41751 直流输入反向
    bool dcOverloadAlarm_41751_bit3 = false; // 41751 直流过载告警
    bool dcOverloadTimeoutFault_41751_bit4 = false; // 41751 直流过载超时故障
    bool dcInputSoftStartFailure_41751_bit5 = false; // 41751 直流输入软启动失败
    bool dcInputElectricControlSwitchOpenCircuit_41751_bit6 = false; // 41751 直流输入电控开关开路
    bool dcInputElectricControlSwitchShortCircuit_41751_bit7 = false; // 41751 直流输入电控开关短路
    bool dcInputGalvanicBreakDeviceHardwareFault_41751_bit8 = false; // 41751 直流输入绝缘检测异常
    bool dcInputSoftGalvanicBreakDeviceFault_41751_bit9 = false; // 41751 直流输入软绝缘检测故障
    bool dcInputDryContactOpenCircuit_41751_bit10 = false; // 41751 直流输入干接点开路

    // ---------- 41752 直流母线 ----------
    bool dcBusOverVoltage_41752_bit0 = false; // 41752 直流母线过压
    bool dcBusUnderVoltage_41752_bit1 = false; // 41752 直流母线欠压
    bool dcBusVoltageUnbalanced_41752_bit2 = false; // 41752 直流母线电压不平衡
    bool dcBusSoftStartFailed_41752_bit3 = false; // 41752 直流母线软启动失败
    bool dcBusGalvanicBreakDeviceOpenCircuit_41752_bit4 = false; // 41752 直流母线绝缘检测开路
    bool dcBusGalvanicBreakDeviceShortCircuit_41752_bit5 = false; // 41752 直流母线绝缘检测短路
    bool dcBusGalvanicBreakDeviceHardwareFault_41752_bit6 = false; // 41752 直流母线绝缘检测硬件故障
    bool dcBusSoftGalvanicBreakDeviceFault_41752_bit7 = false; // 41752 直流母线软绝缘检测故障
    bool dcInsulationDetectAbnormal_41752_bit8 = false; // 41752 直流母线绝缘检测异常

    // ---------- 41753 交流母线 ----------
    bool acBusOverVoltage_41753_bit0 = false;// 41753 交流母线过压
    bool acBusUnderVoltage_41753_bit1 = false; // 41753 交流母线欠压
    bool acBusOverFrequency_41753_bit2 = false; // 41753 交流母线过频
    bool acBusUnderFrequency_41753_bit3 = false; // 41753 交流母线欠频
    bool acBusPhaseReversed_41753_bit4 = false; // 41753 交流母线相序反向
    bool acBusVoltageUnbalance_41753_bit5 = false; // 41753 交流母线电压不平衡
    bool acBusVoltageAbnormal_41753_bit6 = false; // 41753 交流母线电压异常
    bool acBusPhaseLost_41753_bit7 = false; // 41753 交流母线相位丢失
    bool islandingProtection_41753_bit8 = false; // 41753 孤岛保护
    bool pllFailed_41753_bit9 = false; // 41753 PLL失败
    bool acCurrentDcComponentExcess_41753_bit10 = false; // 41753 交流电流直流分量过量
    bool acOverloadAlarm_41753_bit11 = false; // 41753 交流过载告警
    bool acOverloadTimeout_41753_bit12 = false; // 41753 交流过载超时故障
    bool acBusConnectionForbidden_41753_bit13 = false; // 41753 交流母线连接禁止
    bool acBusNPhaseLoss_41753_bit14 = false; // 41753 交流母线N相丢失

    // ---------- 41754 交流开关/软启 ----------
    bool acSoftStartFailed_41754_bit0 = false; // 41754 交流软启动失败
    bool acGalvanicBreakDeviceOpenCircuit_41754_bit1 = false; // 41754 交流绝缘检测开路
    bool acGalvanicBreakDeviceShortCircuit_41754_bit2 = false; // 41754 交流绝缘检测短路
    bool acGalvanicBreakDeviceHardwareFault_41754_bit3 = false; // 41754 交流绝缘检测硬件故障
    bool acSoftStartGalvanicBreakDeviceFault_41754_bit4 = false; // 41754 交流软绝缘检测故障

    // ---------- 41878 电网 ----------
    bool gridOverVoltage_41878_bit0 = false; // 41878 电网过压
    bool gridUnderVoltage_41878_bit1 = false; // 41878 电网欠压
    bool gridOverFrequency_41878_bit2 = false; // 41878 电网过频
    bool gridUnderFrequency_41878_bit3 = false; // 41878 电网欠频
    bool gridPhaseReversed_41878_bit4 = false; // 41878 电网相序反向
    bool gridVoltageUnbalance_41878_bit5 = false; // 41878 电网电压不平衡
    bool gridVoltageAbnormal_41878_bit6 = false; // 41878 电网电压异常
    bool gridPhaseLoss_41878_bit7 = false; // 41878 电网相位丢失
    bool gridNPhaseLoss_41878_bit8 = false; // 41878 电网N相丢失
    bool gridPowerDown_41878_bit9 = false; // 41878 电网断电
    bool gridConnectionForbidden_41878_bit10 = false; // 41878 电网连接禁止

    // ---------- 41879 ----------
    bool gridOverloadAlarm_41879_bit0 = false; // 41879 电网过载告警
    bool gridOverloadTimeoutFault_41879_bit1 = false; // 41879 电网过载超时故障

    // ---------- 41880 离网 ----------
    bool abnormalOffGridVoltage_41880_bit0 = false; // 41880 离网电压异常
    bool offGridVoltageOscillation_41880_bit1 = false; // 41880 离网电压振荡
    bool offGridFrequencyAbnormal_41880_bit2 = false; // 41880 离网频率异常
    bool offGridVoltagePhaseReversed_41880_bit3 = false; // 41880 离网电压相序反向
    bool offGridVoltagePhaseLoss_41880_bit4 = false; // 41880 离网电压相位丢失

    // ---------- 41881 ----------
    bool offGridSoftStartFailure_41881_bit0 = false; // 41881 离网软启动失败
    bool gridOffGridSwitchingError_41881_bit1 = false; // 41881 电网离网开关错误

    // ---------- 42101 充放允许 / BMS 干节点 ----------
    bool chargeEnable_42101_bit0 = false;                      // 允许充电
    bool dischargeEnable_42101_bit1 = false;                   // 允许放电
    bool bmsDryContactChargeForbidden_42101_bit2 = false;      // BMS 干节点禁止充电
    bool bmsDryContactDischargeForbidden_42101_bit3 = false;   // BMS 干节点禁止放电

    // ---------- 42102 BMS 故障 ----------
    bool bmsShutdownFault_42102_bit0 = false;              // BMS 关机故障
    bool bmsCommConnectionTimeout_42102_bit1 = false;      // BMS 通信连接超时
    bool bmsDryContactFault_42102_bit2 = false;            // BMS 干节点故障

    int commErrCount = 0;
    int commFlag = 0; // 0: OK, 1: NG
};


struct SmartenSetData {
    uint16_t pcsOnOff_41379 = 0;                     // PCS 开关机
    uint16_t alarmReset_41378 = 0;                   // 告警复位

    // ---------- 41473~41491 直流/SOC 设定（RW uint16）----------
    uint16_t dcVoltageLowerLimit_41473 = 0;           // 41473 直流下限电压
    uint16_t constantVoltageChargeVoltage_41474 = 0;  // 41474 恒压充电电压
    uint16_t dcOutputVoltage_41475 = 0;               // 41475 直流输出电压
    uint16_t dischargeTerminationVoltage_41478 = 0;   // 41478 放电终止电压
    uint16_t chargeCutoffCurrent_41480 = 0;          // 41480 充电截止电流
    uint16_t batteryProtectionSoc_41489 = 0;       // 41489 强制保电 SOC
    uint16_t dischargeLimitSoc_41490 = 0;       // 41490 禁放 SOC
    uint16_t chargeLimitSoc_41491 = 0;           // 41491 禁充 SOC

    // ---------- 41543~41671 额定/交流接线/并网/功率设定（RW）----------
    uint16_t ratedVoltageLevel_41543 = 0;             // 41543 enum16 额定电压等级
    uint16_t ratedFrequencyLevel_41544 = 0;           // 41544 enum16 额定频率等级
    int16_t activePowerSetting_41546 = 0;             // 41546 int16 有功功率设置（下发倍率 x10）
    int16_t reactivePowerSetting_41547 = 0;           // 41547 int16 无功功率设置（下发倍率 x10）
    uint16_t acConnectType_41580 = 0;              // 41580 enum16 交流接线方式
    uint16_t threePhaseUnbalancedMode_41582 = 0;   // 41582 enum16 功率控制模式
    uint16_t gridInterMode_41671 = 0;              // 41671 enum16 并离网模式
    uint16_t vsgeEnable_41682 = 0;                    // 41682 VSG 使能
    uint16_t vsgeControlMode_41687 = 0;               // 41687 VSG 模式设置
    bool antiBackflowProtection_41696_bit1 = false;     // 41696_bit1 防逆流保护
    bool powerFactorControl_41696_bit2 = false;     // 41696_bit2 功率因数控制
    bool threePhaseUnbalancedMode_41696_bit3 = false;     // 41696_bit3 三相不平衡功率控制模式
    uint16_t energyControlMode_41701 = 0;                   // 41701 能量控制模式

};

const double pcsRatio_value1 = 0.1;
const double pcsRatio_value2 = 0.01;

class PCS_Smarten
{
public:
    PCS_Smarten(const char* device_ip,int device_port, int device_address);
    ~PCS_Smarten();
    void runPcsThread(MySQLConnectionPool& pool);

private:
    bool readRegisterData(MySQLConnectionPool& pool);
    void updatePcsData(MySQLDatabase& db);
    void writeLogicData(MySQLDatabase& db);
    void updatePcsHistoryData();
    void pcsSetData(MySQLDatabase& db);
    void dispatchHmiControlCommands(MySQLDatabase& db);
    bool readHoldingBlock(int startAddr, int count);
    uint32_t decodeUInt32ByOffset(int offset) const;
    uint64_t decodeUInt64ByOffset(int offset) const;

    ModbusTCP modbusclient;
    databaseList Listpcs;
    DatabaseList listpcsHistory;
    influxDB pcsInfluxDb;
    SmartenData smartenData;
    SmartenSetData smartenSetData;
    SmartenSetData himsmartenSetData;
    int pcsRead = 0;
    int pcsWrite = 0;
    uint16_t arr_uint16[30] = {0};
    std::array<uint16_t, 128> arr_holding_registers = {0};
    std::string tableNamePcs = "pcs";
    std::string tableNamePcsSet = "pcsset";
    std::string tableNameLogic = "logic";
    int comAlarmAddr = 2;
    std::chrono::steady_clock::time_point lastHistoryTime{};
};




#endif // PCS_SMARTEN_H