#include "PCS_Smarten.h"
#include "logger.h"
#include "Config.h"
#include <chrono>
#include <cmath>
#include <iostream>
#include <map>
#include <thread>
#include <vector>
//辅助函数
namespace {
// 从寄存器 word 中解析第 bit 位，写入 bool 标志
inline void setBitFlag(bool& flag, uint16_t word, int bit)
{
    flag = (word & (1U << bit)) != 0;
}

// 从 41696 控制字解析三项开关：防逆流 / 功率因数 / 三相不平衡
inline void apply41696ControlBits(SmartenSetData& dst, uint16_t word)
{
    setBitFlag(dst.antiBackflowProtection_41696_bit1, word, 1);
    setBitFlag(dst.powerFactorControl_41696_bit2, word, 2);
    setBitFlag(dst.threePhaseUnbalancedMode_41696_bit3, word, 3);
}

// 41696：bit0 固定为 1，bit4~15 为 0，bit1~3 由界面三项开关组成
inline uint16_t pack41696ControlWord(bool antiBackflow, bool powerFactor, bool unbalanced)
{
    uint16_t v = 0x0001;
    if (antiBackflow) {
        v |= (1U << 1);
    }
    if (powerFactor) {
        v |= (1U << 2);
    }
    if (unbalanced) {
        v |= (1U << 3);
    }
    return v;
}
//映射表
struct PcsFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const SmartenData&);
};
//映射表宏定义
#define PCS_FIELD_BOOL(addr, key, field) \
    { addr, key, [](const SmartenData& d) -> double { return d.field ? 1.0 : 0.0; } }
#define PCS_FIELD_R1(addr, key, field) \
    { addr, key, [](const SmartenData& d) -> double { return static_cast<double>(d.field) * pcsRatio_value1; } }
#define PCS_FIELD_R2(addr, key, field) \
    { addr, key, [](const SmartenData& d) -> double { return static_cast<double>(d.field) * pcsRatio_value2; } }
#define PCS_FIELD_RAW(addr, key, field) \
    { addr, key, [](const SmartenData& d) -> double { return static_cast<double>(d.field); } }

static const PcsFieldMapEntry kPcsFieldMaps[] = {
#include "PCS_Smarten_field_maps.inc"
};
//映射表大小
static constexpr size_t kPcsFieldMapCount = sizeof(kPcsFieldMaps) / sizeof(PcsFieldMapEntry);
static_assert(kPcsFieldMapCount == 189, "PCS field map count mismatch");
}

PCS_Smarten::PCS_Smarten(const char* device_ip, int device_port, int device_address)
    : modbusclient(device_ip, device_port, device_address)
{
}


PCS_Smarten::~PCS_Smarten()
{
    modbusclient.disconnect();
}
//读取寄存器数据
bool PCS_Smarten::readRegisterData(MySQLConnectionPool& pool)
{
    MySQLDatabase db(pool);
    if (!modbusclient.commTest(41382, 1, arr_uint16, tableNamePcs)) {
        smartenData.commErrCount++;
        if (smartenData.commErrCount > 3) {
            smartenData.commFlag = 1;
            db.update(0, smartenData.commFlag, tableNamePcs);
            db.update(comAlarmAddr, smartenData.commFlag, "com_alarm");
            db.update(101, static_cast<double>(smartenData.commFlag), tableNameLogic);
            return false;
        }
    } else {
        smartenData.commErrCount = 0;
        smartenData.commFlag = 0;
    }
    db.update(comAlarmAddr, smartenData.commFlag, "com_alarm");

   
    // 40164~40201：交流母线汇总 + 频率 + 电量 + 环境 + L1（一次读，40168~40170、40181~40188 未用）
    if (readHoldingBlock(40164, 38)) {
        smartenData.acBusTotalActivePower_40164 = static_cast<int16_t>(arr_holding_registers[0]);
        smartenData.acBusTotalApparentPower_40165 = static_cast<int16_t>(arr_holding_registers[1]);
        smartenData.acBusTotalReactivePower_40166 = static_cast<int16_t>(arr_holding_registers[2]);
        smartenData.acBusTotalPowerFactor_40167 = static_cast<int16_t>(arr_holding_registers[3]);
        smartenData.acBusFrequency_40171 = decodeUInt32ByOffset(7);
        smartenData.acBusDischargedEnergy_40173 = decodeUInt64ByOffset(9);
        smartenData.acBusChargedEnergy_40177 = decodeUInt64ByOffset(13);
        smartenData.ambientTemperature_40189 = static_cast<int16_t>(arr_holding_registers[25]);
        smartenData.cabinetTemperature_40190 = static_cast<int16_t>(arr_holding_registers[26]);
        smartenData.moduleTemperature_40193 = static_cast<int16_t>(arr_holding_registers[29]);
        smartenData.acBusL1ActivePower_40195 = static_cast<int16_t>(arr_holding_registers[31]);
        smartenData.acBusL1ApparentPower_40196 = static_cast<int16_t>(arr_holding_registers[32]);
        smartenData.acBusL1ReactivePower_40197 = static_cast<int16_t>(arr_holding_registers[33]);
        smartenData.acBusL1PowerFactor_40198 = static_cast<int16_t>(arr_holding_registers[34]);
        smartenData.acBusL1Current_40199 = static_cast<int16_t>(arr_holding_registers[35]);
        smartenData.acBusL1L2LineVoltage_40200 = static_cast<int16_t>(arr_holding_registers[36]);
        smartenData.acBusL1NVoltage_40201 = static_cast<int16_t>(arr_holding_registers[37]);
    }

    // 40218~40247：交流母线 L2 + L3（一次读，中间 40225~40240 未用）
    if (readHoldingBlock(40218, 30)) {
        smartenData.acBusL2ActivePower_40218 = static_cast<int16_t>(arr_holding_registers[0]);
        smartenData.acBusL2ApparentPower_40219 = static_cast<int16_t>(arr_holding_registers[1]);
        smartenData.acBusL2ReactivePower_40220 = static_cast<int16_t>(arr_holding_registers[2]);
        smartenData.acBusL2PowerFactor_40221 = static_cast<int16_t>(arr_holding_registers[3]);
        smartenData.acBusL2Current_40222 = static_cast<int16_t>(arr_holding_registers[4]);
        smartenData.acBusL2L3LineVoltage_40223 = static_cast<int16_t>(arr_holding_registers[5]);
        smartenData.acBusL2NVoltage_40224 = static_cast<int16_t>(arr_holding_registers[6]);
        smartenData.acBusL3ActivePower_40241 = static_cast<int16_t>(arr_holding_registers[23]);
        smartenData.acBusL3ApparentPower_40242 = static_cast<int16_t>(arr_holding_registers[24]);
        smartenData.acBusL3ReactivePower_40243 = static_cast<int16_t>(arr_holding_registers[25]);
        smartenData.acBusL3PowerFactor_40244 = static_cast<int16_t>(arr_holding_registers[26]);
        smartenData.acBusL3Current_40245 = static_cast<int16_t>(arr_holding_registers[27]);
        smartenData.acBusL3L1LineVoltage_40246 = static_cast<int16_t>(arr_holding_registers[28]);
        smartenData.acBusL3NVoltage_40247 = static_cast<int16_t>(arr_holding_registers[29]);
    }


    //41842~41849(直流侧)
    if (readHoldingBlock(41842, 11)) {
        smartenData.dcPower_41842 = static_cast<int16_t>(arr_holding_registers[0]);//直流功率
        smartenData.dcCurrent_41843 = static_cast<int16_t>(arr_holding_registers[1]);//直流电流
        smartenData.dcInputVoltage_41844 = static_cast<int16_t>(arr_holding_registers[2]);//直流输入电压
        smartenData.dcDischargeEnergy_41845 = decodeUInt64ByOffset(3);//直流放电电量
        smartenData.dcChargeEnergy_41849 = decodeUInt64ByOffset(7);//直流充电电量
    }

    //41885~41922(电网侧)
    if (readHoldingBlock(41885, 38)) {
        smartenData.gridTotalPowerFactor_41885 = static_cast<int16_t>(arr_holding_registers[0]);//电网总功率因数
        smartenData.gridTotalActivePower_41886 = static_cast<int16_t>(arr_holding_registers[1]);//电网总有功
        smartenData.gridTotalReactivePower_41887 = static_cast<int16_t>(arr_holding_registers[2]);//电网总无功
        smartenData.gridTotalApparentPower_41888 = static_cast<int16_t>(arr_holding_registers[3]);//电网总视在
        smartenData.gridFrequency_41889 = static_cast<int16_t>(arr_holding_registers[4]);//电网频率
        smartenData.gridL1PowerFactor_41893 = static_cast<int16_t>(arr_holding_registers[8]);//L1 功率因数
        smartenData.gridL1ActivePower_41894 = static_cast<int16_t>(arr_holding_registers[9]);//L1 有功
        smartenData.gridL1ReactivePower_41895 = static_cast<int16_t>(arr_holding_registers[10]);//L1 无功
        smartenData.gridL1ApparentPower_41896 = static_cast<int16_t>(arr_holding_registers[11]);//L1 视在
        smartenData.gridL1Current_41897 = static_cast<int16_t>(arr_holding_registers[12]);//L1 电流
        smartenData.gridL2PowerFactor_41901 = static_cast<int16_t>(arr_holding_registers[16]);//L2 功率因数
        smartenData.gridL2ActivePower_41902 = static_cast<int16_t>(arr_holding_registers[17]);//L2 有功
        smartenData.gridL2ReactivePower_41903 = static_cast<int16_t>(arr_holding_registers[18]);//L2 无功
        smartenData.gridL2ApparentPower_41904 = static_cast<int16_t>(arr_holding_registers[19]);//L2 视在
        smartenData.gridL2Current_41905 = static_cast<int16_t>(arr_holding_registers[20]);//L2 电流
        smartenData.gridL3PowerFactor_41909 = static_cast<int16_t>(arr_holding_registers[24]);//L3 功率因数
        smartenData.gridL3ActivePower_41910 = static_cast<int16_t>(arr_holding_registers[25]);//L3 有功
        smartenData.gridL3ReactivePower_41911 = static_cast<int16_t>(arr_holding_registers[26]);//L3 无功
        smartenData.gridL3ApparentPower_41912 = static_cast<int16_t>(arr_holding_registers[27]);//L3 视在
        smartenData.gridL3Current_41913 = static_cast<int16_t>(arr_holding_registers[28]);//L3 电流
        smartenData.gridL1L2LineVoltage_41917 = static_cast<int16_t>(arr_holding_registers[32]);//L1-L2 线电压(AB)
        smartenData.gridL2L3LineVoltage_41918 = static_cast<int16_t>(arr_holding_registers[33]);//L2-L3 线电压(BC)
        smartenData.gridL3L1LineVoltage_41919 = static_cast<int16_t>(arr_holding_registers[34]);//L3-L1 线电压(CA)
        smartenData.gridL1NVoltage_41920 = static_cast<int16_t>(arr_holding_registers[35]);//L1-N 相电压(AN)
        smartenData.gridL2NVoltage_41921 = static_cast<int16_t>(arr_holding_registers[36]);//L2-N 相电压(BN)
        smartenData.gridL3NVoltage_41922 = static_cast<int16_t>(arr_holding_registers[37]);//L3-N 相电压(CN)
    }

    //41946~41971(负载侧)
    if (readHoldingBlock(41946, 26)) {
        smartenData.loadTotalPowerFactor_41946 = static_cast<int16_t>(arr_holding_registers[0]);//总功率因数
        smartenData.loadTotalActivePower_41947 = static_cast<int16_t>(arr_holding_registers[1]);//总有功
        smartenData.loadTotalReactivePower_41948 = static_cast<int16_t>(arr_holding_registers[2]);//总无功
        smartenData.loadTotalApparentPower_41949 = static_cast<int16_t>(arr_holding_registers[3]);//总视在
        smartenData.loadFrequency_41950 = static_cast<int16_t>(arr_holding_registers[4]);//频率
        smartenData.loadL1PowerFactor_41951 = static_cast<int16_t>(arr_holding_registers[5]);//L1 功率因数
        smartenData.loadL1ActivePower_41952 = static_cast<int16_t>(arr_holding_registers[6]);//L1 有功
        smartenData.loadL1ReactivePower_41953 = static_cast<int16_t>(arr_holding_registers[7]);//L1 无功
        smartenData.loadL1ApparentPower_41954 = static_cast<int16_t>(arr_holding_registers[8]);//L1 视在
        smartenData.loadL1Current_41955 = static_cast<int16_t>(arr_holding_registers[9]);//L1 电流
        smartenData.loadL2PowerFactor_41956 = static_cast<int16_t>(arr_holding_registers[10]);//L2 功率因数
        smartenData.loadL2ActivePower_41957 = static_cast<int16_t>(arr_holding_registers[11]);//L2 有功
        smartenData.loadL2ReactivePower_41958 = static_cast<int16_t>(arr_holding_registers[12]);//L2 无功
        smartenData.loadL2ApparentPower_41959 = static_cast<int16_t>(arr_holding_registers[13]);//L2 视在
        smartenData.loadL2Current_41960 = static_cast<int16_t>(arr_holding_registers[14]);//L2 电流
        smartenData.loadL3PowerFactor_41961 = static_cast<int16_t>(arr_holding_registers[15]);//L3 功率因数
        smartenData.loadL3ActivePower_41962 = static_cast<int16_t>(arr_holding_registers[16]);//L3 有功
        smartenData.loadL3ReactivePower_41963 = static_cast<int16_t>(arr_holding_registers[17]);//L3 无功
        smartenData.loadL3ApparentPower_41964 = static_cast<int16_t>(arr_holding_registers[18]);//L3 视在
        smartenData.loadL3Current_41965 = static_cast<int16_t>(arr_holding_registers[19]);//L3 电流
        smartenData.loadL1L2LineVoltage_41966 = static_cast<int16_t>(arr_holding_registers[20]);//L1-L2 线电压(AB)
        smartenData.loadL2L3LineVoltage_41967 = static_cast<int16_t>(arr_holding_registers[21]);//L2-L3 线电压(BC)
        smartenData.loadL3L1LineVoltage_41968 = static_cast<int16_t>(arr_holding_registers[22]);//L3-L1 线电压(CA)
        smartenData.loadL1NVoltage_41969 = static_cast<int16_t>(arr_holding_registers[23]);//L1-N 相电压(AN)
        smartenData.loadL2NVoltage_41970 = static_cast<int16_t>(arr_holding_registers[24]);//L2-N 相电压(BN)
        smartenData.loadL3NVoltage_41971 = static_cast<int16_t>(arr_holding_registers[25]);//L3-N 相电压(CN)
    }

    // 41725~41734：状态字 + 软硬件/通讯告警（一次读）
    if (readHoldingBlock(41725, 10)) {
        const uint16_t* r = arr_holding_registers.data();
        setBitFlag(smartenData.faultState_41725_bit0, r[0], 0);//故障状态 
        setBitFlag(smartenData.alarmState_41725_bit1, r[0], 1);//告警状态
        setBitFlag(smartenData.totalFanFault_41725_bit2, r[0], 2);//总风扇故障
        setBitFlag(smartenData.totalAuxPowerFault_41725_bit3, r[0], 3);//总辅助电源故障
        setBitFlag(smartenData.totalOverTemperatureFault_41725_bit4, r[0], 4);//总过温故障
        setBitFlag(smartenData.totalModuleOverCurrentFault_41725_bit5, r[0], 5);//总模块过流故障
        setBitFlag(smartenData.totalModuleCurrentLimitAlarm_41725_bit6, r[0], 6);//总模块电流限制告警
        setBitFlag(smartenData.ambientOverTemperatureFault_41725_bit7, r[0], 7);//环境过温故障
        setBitFlag(smartenData.moduleCurrentUnbalanceFault_41725_bit8, r[0], 8);//模块电流不平衡故障

        setBitFlag(smartenData.startStopState_41726_bit1, r[1], 1);//启动停止状态
        setBitFlag(smartenData.startingState_41726_bit2, r[1], 2);//启动状态
        setBitFlag(smartenData.standbyState_41726_bit3, r[1], 3);//待机状态
        setBitFlag(smartenData.onGridState_41726_bit4, r[1], 4);//并网状态
        setBitFlag(smartenData.offGridState_41726_bit5, r[1], 5);//离网状态
        setBitFlag(smartenData.chargeState_41726_bit6, r[1], 6);//充电状态
        setBitFlag(smartenData.dischargeState_41726_bit7, r[1], 7);//放电状态
        setBitFlag(smartenData.deratingState_41726_bit8, r[1], 8);//降容状态
        setBitFlag(smartenData.fullChargeState_41726_bit9, r[1], 9);//满充状态
        setBitFlag(smartenData.emptyDischargeState_41726_bit10, r[1], 10);//空放状态
        setBitFlag(smartenData.equalizationChargeState_41726_bit11, r[1], 11);//均衡充电状态
        setBitFlag(smartenData.floatChargeState_41726_bit12, r[1], 12);//浮充状态

        setBitFlag(smartenData.droopState_41727_bit1, r[2], 1);//下垂状态
        setBitFlag(smartenData.synchronizationState_41727_bit2, r[2], 2);//同步状态
        setBitFlag(smartenData.synchronizationDoneState_41727_bit3, r[2], 3);//同步完成状态

        setBitFlag(smartenData.emergencyShutdown_41728_bit0, r[3], 0);//紧急关机

        setBitFlag(smartenData.dspInitializingAbnormal_41729_bit0, r[4], 0);//DSP初始化异常
        setBitFlag(smartenData.dspVersionAbnormal_41729_bit1, r[4], 1);//DSP版本异常
        setBitFlag(smartenData.dspParameterMismatch_41729_bit2, r[4], 2);//DSP参数不匹配
        setBitFlag(smartenData.cpldVersionAbnormal_41729_bit3, r[4], 3);//CPLD版本异常
        setBitFlag(smartenData.flashAbnormal_41729_bit4, r[4], 4);//闪存异常

        setBitFlag(smartenData.monitorInitializingAbnormal_41730_bit0, r[5], 0);//监控初始化异常
        setBitFlag(smartenData.monitorParameterMismatch_41730_bit1, r[5], 1);//监控参数不匹配

        setBitFlag(smartenData.hardwareVersionError_41731_bit0, r[6], 0);//硬件版本异常
        setBitFlag(smartenData.idReduplicatedFault_41731_bit1, r[6], 1);//ID重复故障

        setBitFlag(smartenData.calibrationParameterError_41732_bit0, r[7], 0);//校准参数异常
        setBitFlag(smartenData.samplingZeroPointError_41732_bit1, r[7], 1);//采样零点异常

        setBitFlag(smartenData.canACommunicationFault_41733_bit0, r[8], 0);//CAN A通讯异常
        setBitFlag(smartenData.canBCommunicationFault_41733_bit1, r[8], 1);//CAN B通讯异常
        setBitFlag(smartenData.canCCommunicationFault_41733_bit2, r[8], 2);//CAN C通讯异常
        setBitFlag(smartenData.canDCommunicationFault_41733_bit3, r[8], 3);//CAN D通讯异常
        setBitFlag(smartenData.rs485_1CommunicationFault_41733_bit4, r[8], 4);//RS485_1通讯异常
        setBitFlag(smartenData.rs485_2CommunicationFault_41733_bit5, r[8], 5);//RS485_2通讯异常
        setBitFlag(smartenData.rs485_3CommunicationFault_41733_bit6, r[8], 6);//RS485_3通讯异常
        setBitFlag(smartenData.rs485_4CommunicationFault_41733_bit7, r[8], 7);//RS485_4通讯异常
        setBitFlag(smartenData.auxiliaryBoardCommunicationFault_41733_bit8, r[8], 8);//辅助板通讯异常
        setBitFlag(smartenData.spiCommunicationFault_41733_bit9, r[8], 9);//SPI通讯异常
        setBitFlag(smartenData.emsCommConnectionTimeout_41733_bit10, r[8], 10);//EMS通讯超时
        setBitFlag(smartenData.rs485CommunicationFault_41733_bit11, r[8], 11);//RS485通讯异常

        setBitFlag(smartenData.synchronizationSignal1Fault_41734_bit0, r[9], 0);//同步信号1异常
        setBitFlag(smartenData.synchronizationSignal2Fault_41734_bit1, r[9], 1);//同步信号2异常

        // 故障 > 告警 > 运行 > 停止；避免充放电位与故障位同时为真时误显示 Running
        const bool isStopLike = smartenData.startStopState_41726_bit1
            || smartenData.startingState_41726_bit2
            || smartenData.standbyState_41726_bit3
            || smartenData.fullChargeState_41726_bit9
            || smartenData.emptyDischargeState_41726_bit10
            || smartenData.emergencyShutdown_41728_bit0;
        const bool isRunningLike = smartenData.chargeState_41726_bit6
            || smartenData.dischargeState_41726_bit7
            || smartenData.deratingState_41726_bit8
            || smartenData.equalizationChargeState_41726_bit11
            || smartenData.floatChargeState_41726_bit12;

        if (smartenData.faultState_41725_bit0) {
            smartenData.runState = static_cast<uint16_t>(PCS_SmartenRunState::Fault);
        } else if (smartenData.alarmState_41725_bit1) {
            smartenData.runState = static_cast<uint16_t>(PCS_SmartenRunState::Alarm);
        } else if (isRunningLike) {
            smartenData.runState = static_cast<uint16_t>(PCS_SmartenRunState::Running);
        } else if (isStopLike) {
            smartenData.runState = static_cast<uint16_t>(PCS_SmartenRunState::Stop);
        } else {
            smartenData.runState = static_cast<uint16_t>(PCS_SmartenRunState::Stop);
        }

        if(smartenData.onGridState_41726_bit4 == true){
            smartenData.gridState = static_cast<uint16_t>(PCS_SmartenOnOffGridState::GridOn);
        }else if(smartenData.offGridState_41726_bit5 == true){
            smartenData.gridState = static_cast<uint16_t>(PCS_SmartenOnOffGridState::GridOff);
        } else {
            const uint16_t unknown =
                static_cast<uint16_t>(PCS_SmartenOnOffGridState::Unknown);
            if (smartenData.gridState != unknown) {
                std::cerr << "PCS并离网状态未知：41726 bit4/bit5 均为 false" << std::endl;
            }
            smartenData.gridState = unknown;
        }
    }

    // 41746~41754 降额 + 直流/交流母线等告警（41747~41750 未用）
    if (readHoldingBlock(41746, 9)) {
        const uint16_t* r = arr_holding_registers.data();
        setBitFlag(smartenData.totalDeratingAbnormal_41746_bit0, r[0], 0);//总降额异常
        setBitFlag(smartenData.moduleOvertempDerating_41746_bit1, r[0], 1);//模块过温降额
        setBitFlag(smartenData.cabinetOvertempDerating_41746_bit2, r[0], 2);//柜体过温降额
        setBitFlag(smartenData.ambientOvertempDerating_41746_bit3, r[0], 3);//环境过温降额

        setBitFlag(smartenData.dcInputOvervoltage_41751_bit0, r[5], 0);//直流输入过压
        setBitFlag(smartenData.dcInputUndervoltage_41751_bit1, r[5], 1);//直流输入欠压
        setBitFlag(smartenData.dcInputReverse_41751_bit2, r[5], 2);//直流输入反向
        setBitFlag(smartenData.dcOverloadAlarm_41751_bit3, r[5], 3);//直流过载告警
        setBitFlag(smartenData.dcOverloadTimeoutFault_41751_bit4, r[5], 4);//直流过载超时故障
        setBitFlag(smartenData.dcInputSoftStartFailure_41751_bit5, r[5], 5);//直流输入软启动失败
        setBitFlag(smartenData.dcInputElectricControlSwitchOpenCircuit_41751_bit6, r[5], 6);//直流输入电控开关开路
        setBitFlag(smartenData.dcInputElectricControlSwitchShortCircuit_41751_bit7, r[5], 7);//直流输入电控开关短路
        setBitFlag(smartenData.dcInputGalvanicBreakDeviceHardwareFault_41751_bit8, r[5], 8);//直流输入绝缘检测异常
        setBitFlag(smartenData.dcInputSoftGalvanicBreakDeviceFault_41751_bit9, r[5], 9);//直流输入软绝缘检测故障
        setBitFlag(smartenData.dcInputDryContactOpenCircuit_41751_bit10, r[5], 10);//直流输入干接点开路

        setBitFlag(smartenData.dcBusOverVoltage_41752_bit0, r[6], 0);//直流母线过压
        setBitFlag(smartenData.dcBusUnderVoltage_41752_bit1, r[6], 1);//直流母线欠压
        setBitFlag(smartenData.dcBusVoltageUnbalanced_41752_bit2, r[6], 2);//直流母线电压不平衡
        setBitFlag(smartenData.dcBusSoftStartFailed_41752_bit3, r[6], 3);//直流母线软启动失败
        setBitFlag(smartenData.dcBusGalvanicBreakDeviceOpenCircuit_41752_bit4, r[6], 4);//直流母线绝缘检测开路
        setBitFlag(smartenData.dcBusGalvanicBreakDeviceShortCircuit_41752_bit5, r[6], 5);//直流母线绝缘检测短路
        setBitFlag(smartenData.dcBusGalvanicBreakDeviceHardwareFault_41752_bit6, r[6], 6);//直流母线绝缘检测硬件故障
        setBitFlag(smartenData.dcBusSoftGalvanicBreakDeviceFault_41752_bit7, r[6], 7);//直流母线软绝缘检测故障
        setBitFlag(smartenData.dcInsulationDetectAbnormal_41752_bit8, r[6], 8);//直流母线绝缘检测异常

        setBitFlag(smartenData.acBusOverVoltage_41753_bit0, r[7], 0);//交流母线过压
        setBitFlag(smartenData.acBusUnderVoltage_41753_bit1, r[7], 1);//交流母线欠压
        setBitFlag(smartenData.acBusOverFrequency_41753_bit2, r[7], 2);//交流母线过频
        setBitFlag(smartenData.acBusUnderFrequency_41753_bit3, r[7], 3);//交流母线欠频
        setBitFlag(smartenData.acBusPhaseReversed_41753_bit4, r[7], 4);//交流母线相序反向
        setBitFlag(smartenData.acBusVoltageUnbalance_41753_bit5, r[7], 5);//交流母线电压不平衡
        setBitFlag(smartenData.acBusVoltageAbnormal_41753_bit6, r[7], 6);//交流母线电压异常
        setBitFlag(smartenData.acBusPhaseLost_41753_bit7, r[7], 7);//交流母线相位丢失
        setBitFlag(smartenData.islandingProtection_41753_bit8, r[7], 8);//孤岛保护
        setBitFlag(smartenData.pllFailed_41753_bit9, r[7], 9);//PLL失败
        setBitFlag(smartenData.acCurrentDcComponentExcess_41753_bit10, r[7], 10);//交流电流直流分量过量
        setBitFlag(smartenData.acOverloadAlarm_41753_bit11, r[7], 11);//交流过载告警
        setBitFlag(smartenData.acOverloadTimeout_41753_bit12, r[7], 12);//交流过载超时故障
        setBitFlag(smartenData.acBusConnectionForbidden_41753_bit13, r[7], 13);//交流母线连接禁止
        setBitFlag(smartenData.acBusNPhaseLoss_41753_bit14, r[7], 14);//交流母线N相丢失

        setBitFlag(smartenData.acSoftStartFailed_41754_bit0, r[8], 0);//交流软启动失败
        setBitFlag(smartenData.acGalvanicBreakDeviceOpenCircuit_41754_bit1, r[8], 1);//交流绝缘检测开路
        setBitFlag(smartenData.acGalvanicBreakDeviceShortCircuit_41754_bit2, r[8], 2);//交流绝缘检测短路
        setBitFlag(smartenData.acGalvanicBreakDeviceHardwareFault_41754_bit3, r[8], 3);//交流绝缘检测硬件故障
        setBitFlag(smartenData.acSoftStartGalvanicBreakDeviceFault_41754_bit4, r[8], 4);//交流软绝缘检测故障
    }

    // 41878~41881 电网/离网告警
    if (readHoldingBlock(41878, 4)) {
        const uint16_t* r = arr_holding_registers.data();
        setBitFlag(smartenData.gridOverVoltage_41878_bit0, r[0], 0);//电网过压
        setBitFlag(smartenData.gridUnderVoltage_41878_bit1, r[0], 1);//电网欠压
        setBitFlag(smartenData.gridOverFrequency_41878_bit2, r[0], 2);//电网过频
        setBitFlag(smartenData.gridUnderFrequency_41878_bit3, r[0], 3);//电网欠频
        setBitFlag(smartenData.gridPhaseReversed_41878_bit4, r[0], 4);//电网相序反向
        setBitFlag(smartenData.gridVoltageUnbalance_41878_bit5, r[0], 5);//电网电压不平衡
        setBitFlag(smartenData.gridVoltageAbnormal_41878_bit6, r[0], 6);//电网电压异常
        setBitFlag(smartenData.gridPhaseLoss_41878_bit7, r[0], 7);//电网相位丢失
        setBitFlag(smartenData.gridNPhaseLoss_41878_bit8, r[0], 8);//电网N相丢失
        setBitFlag(smartenData.gridPowerDown_41878_bit9, r[0], 9);//电网断电
        setBitFlag(smartenData.gridConnectionForbidden_41878_bit10, r[0], 10);//电网连接禁止

        setBitFlag(smartenData.gridOverloadAlarm_41879_bit0, r[1], 0);//电网过载告警
        setBitFlag(smartenData.gridOverloadTimeoutFault_41879_bit1, r[1], 1);//电网过载超时故障

        setBitFlag(smartenData.abnormalOffGridVoltage_41880_bit0, r[2], 0);//离网电压异常
        setBitFlag(smartenData.offGridVoltageOscillation_41880_bit1, r[2], 1);//离网电压振荡
        setBitFlag(smartenData.offGridFrequencyAbnormal_41880_bit2, r[2], 2);//离网频率异常
        setBitFlag(smartenData.offGridVoltagePhaseReversed_41880_bit3, r[2], 3);//离网电压相序反向
        setBitFlag(smartenData.offGridVoltagePhaseLoss_41880_bit4, r[2], 4);//离网电压相位丢失

        setBitFlag(smartenData.offGridSoftStartFailure_41881_bit0, r[3], 0);//离网软启动失败
        setBitFlag(smartenData.gridOffGridSwitchingError_41881_bit1, r[3], 1);//电网离网开关错误
    }

    // 42101~42102 BMS / 充放允许
    if (readHoldingBlock(42101, 2)) {
        const uint16_t* r = arr_holding_registers.data();
        setBitFlag(smartenData.chargeEnable_42101_bit0, r[0], 0);//允许充电
        setBitFlag(smartenData.dischargeEnable_42101_bit1, r[0], 1);//允许放电
        setBitFlag(smartenData.bmsDryContactChargeForbidden_42101_bit2, r[0], 2);//BMS 干节点禁止充电
        setBitFlag(smartenData.bmsDryContactDischargeForbidden_42101_bit3, r[0], 3);//BMS 干节点禁止放电

        setBitFlag(smartenData.bmsShutdownFault_42102_bit0, r[1], 0);//BMS 关机故障
        setBitFlag(smartenData.bmsCommConnectionTimeout_42102_bit1, r[1], 1);//BMS 通信连接超时
        setBitFlag(smartenData.bmsDryContactFault_42102_bit2, r[1], 2);//BMS 干节点故障
    }

    if(modbusclient.readRegisters(41378, 2, arr_uint16) != -1){
        smartenSetData.alarmReset_41378 = arr_uint16[0];
        smartenSetData.pcsOnOff_41379 = arr_uint16[1];
    }
    if (modbusclient.readRegisters(41546, 2, arr_uint16) != -1) {
        smartenSetData.activePowerSetting_41546 = static_cast<int16_t>(arr_uint16[0]);
        smartenSetData.reactivePowerSetting_41547 = static_cast<int16_t>(arr_uint16[1]);
    }

    if (modbusclient.readRegisters(41671, 1, arr_uint16) != -1) {
        smartenSetData.gridInterMode_41671 = arr_uint16[0];
    }

    //下发人机界面控制指令
    dispatchHmiControlCommands(db);

    // 请求式参数同步通道（pcsset addr 19 读设备 / 20 写设备）
    pcsSetData(db);
    //更新PCS数据到数据库
    updatePcsData(db);
    //更新PCS历史数据
    auto now = std::chrono::steady_clock::now();
    if (lastHistoryTime == std::chrono::steady_clock::time_point{}
        || now - lastHistoryTime >= std::chrono::seconds(30)) {
        updatePcsHistoryData();
        lastHistoryTime = now;
    }
    return true;
}


void PCS_Smarten::dispatchHmiControlCommands(MySQLDatabase& db)
{
    himsmartenSetData.alarmReset_41378 = db.select(602, "qt");//故障复位标志
    static const std::vector<int> kDataTotalCmdAddrs{108, 109, 113, 115};//人机界面指令地址
    const std::map<int, float> hmiCmd = db.selectMultipleData("data_total", kDataTotalCmdAddrs);//人机界面指令
    himsmartenSetData.pcsOnOff_41379 = static_cast<uint16_t>(hmiCmd.at(108));//开关机指令
    himsmartenSetData.gridInterMode_41671 = static_cast<uint16_t>(hmiCmd.at(115));//并网离网指令
    const int targetP = static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(109)) * 10));//有功功率指令
    const int targetQ = static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(113)) * 10));//无功功率指令
    himsmartenSetData.activePowerSetting_41546 = static_cast<int16_t>(targetP);//有功功率指令
    himsmartenSetData.reactivePowerSetting_41547 = static_cast<int16_t>(targetQ);//无功功率指令

    const int hmiOnOff = static_cast<int>(himsmartenSetData.pcsOnOff_41379);
    const int actualOnOff = static_cast<int>(smartenSetData.pcsOnOff_41379);
    if (hmiOnOff == 1 && actualOnOff != 1) {//如果人机界面开关机指令为1，且设备开关机指令为0，则下发开机指令
        std::cout << "PCS开机指令下发" << std::endl;
        LOG_ACTION("PCS开机");
        modbusclient.writeRegister(41379, 1, "pcsset");
    } else if (hmiOnOff == 0 && actualOnOff != 0) {//如果人机界面开关机指令为0，且设备开关机指令为1，则下发关机指令
        std::cout << "PCS关机指令下发" << std::endl;
        LOG_ACTION("PCS关机");
        modbusclient.writeRegister(41379, 0, "pcsset");
    }

    const int hmiGridMode = static_cast<int>(himsmartenSetData.gridInterMode_41671);
    const int actualGridMode = static_cast<int>(smartenSetData.gridInterMode_41671);
    //如果人机界面并网离网指令为0，且设备并网离网指令为1，则下发并网指令
    if (hmiGridMode == 0 && actualGridMode != 0) {//如果人机界面并网离网指令为0，且设备并网离网指令为1，则下发并网指令
        std::cout << "PCS并网指令下发" << std::endl;
        LOG_ACTION("PCS并网");
        modbusclient.writeRegister(41671, 0, "pcsset");//并网指令
    } else if (hmiGridMode != 0 && actualGridMode != hmiGridMode) {//如果人机界面并网离网指令不为0，且设备并网离网指令不为人机界面并网离网指令，则下发离网指令
        std::cout << "PCS离网指令下发" << std::endl;
        LOG_ACTION("PCS离网");
        modbusclient.writeRegister(41671, 1, "pcsset");//离网指令
    }
    //如果人机界面故障复位标志为1，则下发故障复位指令
    if (himsmartenSetData.alarmReset_41378 == 1) {
        modbusclient.writeRegister(41378, 0, "pcsset");//故障复位指令
        std::cout << "PCS告警复位指令下发" << std::endl;
        LOG_ACTION("PCS告警复位");
        db.update(602, 0, "qt");//故障复位标志
    }
    //如果人机界面有功功率指令与设备有功功率指令差值大于10，则下发有功功率指令
    constexpr int kPcsPowerCmdWriteDeadband = 10;
    const int curP = static_cast<int>(smartenSetData.activePowerSetting_41546);
    const int diffP = targetP - curP;
    if (diffP > kPcsPowerCmdWriteDeadband || diffP < -kPcsPowerCmdWriteDeadband) {
        modbusclient.writeRegister(41546, static_cast<uint16_t>(targetP), "pcsset");
        std::cout << "PCS有功功率指令下发:" << targetP / 10.0 << " kW" << std::endl;
        LOG_ACTION("PCS有功功率指令下发:" + std::to_string(targetP / 10.0) + " kW");
    }
    //如果人机界面无功功率指令与设备无功功率指令差值大于10，则下发无功功率指令
    const int curQ = static_cast<int>(smartenSetData.reactivePowerSetting_41547);
    const int diffQ = targetQ - curQ;
    if (diffQ > kPcsPowerCmdWriteDeadband || diffQ < -kPcsPowerCmdWriteDeadband) {
        modbusclient.writeRegister(41547, static_cast<uint16_t>(targetQ), "pcsset");
        std::cout << "PCS无功功率指令下发:" << targetQ / 10.0 << " kvar" << std::endl;
        LOG_ACTION("PCS无功功率指令下发:" + std::to_string(targetQ / 10.0) + " kvar");
    }
}

//更新PCS数据到寄存器
void PCS_Smarten::pcsSetData(MySQLDatabase& db){
    struct SetMap {
        int regAddr;
        int dbAddr;
        int readOffset;
        int scale;
        uint16_t SmartenSetData::* field;
    };

    static const SetMap kSetMaps[] = {
        {41473, 1, 0, 10, &SmartenSetData::dcVoltageLowerLimit_41473},
        {41474, 2, 1, 10, &SmartenSetData::constantVoltageChargeVoltage_41474},
        {41475, 3, 2, 10, &SmartenSetData::dcOutputVoltage_41475},
        {41478, 4, 5, 10, &SmartenSetData::dischargeTerminationVoltage_41478},
        {41480, 5, 7, 10, &SmartenSetData::chargeCutoffCurrent_41480},
        {41489, 6, 16, 10, &SmartenSetData::batteryProtectionSoc_41489},
        {41490, 7, 17, 10, &SmartenSetData::dischargeLimitSoc_41490},
        {41491, 8, 18, 10, &SmartenSetData::chargeLimitSoc_41491},
        {41682, 9, -1, 1, &SmartenSetData::vsgeEnable_41682},
        {41687, 10, -1, 1, &SmartenSetData::vsgeControlMode_41687},
        {41580, 11, -1, 1, &SmartenSetData::acConnectType_41580},
        {41582, 12, -1, 1, &SmartenSetData::threePhaseUnbalancedMode_41582},
        {41543, 13, -1, 1, &SmartenSetData::ratedVoltageLevel_41543},
        {41544, 14, -1, 1, &SmartenSetData::ratedFrequencyLevel_41544},
        {41701, 18, -1, 1, &SmartenSetData::energyControlMode_41701},
    };

    constexpr int kReg41696 = 41696;

    pcsRead = db.select(19, "pcsset");
    pcsWrite = db.select(20, "pcsset");
    if (pcsRead != 1 && pcsWrite != 1) {
        return;
    }

    auto refreshCurrentSetData = [this, kReg41696]() -> bool {
        if (modbusclient.readRegisters(41473, 19, arr_uint16) == -1) {
            return false;
        }
        for (const auto& m : kSetMaps) {
            if (m.readOffset >= 0) {
                smartenSetData.*(m.field) = arr_uint16[m.readOffset];
            }
        }
        for (const auto& m : kSetMaps) {
            if (m.readOffset < 0) {
                if (modbusclient.readRegisters(m.regAddr, 1, arr_uint16) == -1) {
                    return false;
                }
                smartenSetData.*(m.field) = arr_uint16[0];
            }
        }
        if (modbusclient.readRegisters(kReg41696, 1, arr_uint16) == -1) {
            return false;
        }
        apply41696ControlBits(smartenSetData, arr_uint16[0]);
        return true;
    };

    if (!refreshCurrentSetData()) {
        return;
    }

    if (pcsRead == 1) {
        Listpcs.clearData();
        Listpcs.addData(1, smartenSetData.dcVoltageLowerLimit_41473 * pcsRatio_value1);
        Listpcs.addData(2, smartenSetData.constantVoltageChargeVoltage_41474 * pcsRatio_value1);
        Listpcs.addData(3, smartenSetData.dcOutputVoltage_41475 * pcsRatio_value1);
        Listpcs.addData(4, smartenSetData.dischargeTerminationVoltage_41478 * pcsRatio_value1);
        Listpcs.addData(5, smartenSetData.chargeCutoffCurrent_41480 * pcsRatio_value1);
        Listpcs.addData(6, smartenSetData.batteryProtectionSoc_41489 * pcsRatio_value1);
        Listpcs.addData(7, smartenSetData.dischargeLimitSoc_41490 * pcsRatio_value1);
        Listpcs.addData(8, smartenSetData.chargeLimitSoc_41491 * pcsRatio_value1);
        Listpcs.addData(9, smartenSetData.vsgeEnable_41682);
        Listpcs.addData(10, smartenSetData.vsgeControlMode_41687);
        Listpcs.addData(11, smartenSetData.acConnectType_41580);
        Listpcs.addData(12, smartenSetData.threePhaseUnbalancedMode_41582);
        Listpcs.addData(13, smartenSetData.ratedVoltageLevel_41543);
        Listpcs.addData(14, smartenSetData.ratedFrequencyLevel_41544);
        Listpcs.addData(15, smartenSetData.antiBackflowProtection_41696_bit1 ? 1.0 : 0.0);
        Listpcs.addData(16, smartenSetData.powerFactorControl_41696_bit2 ? 1.0 : 0.0);
        Listpcs.addData(17, smartenSetData.threePhaseUnbalancedMode_41696_bit3 ? 1.0 : 0.0);
        Listpcs.addData(18, smartenSetData.energyControlMode_41701);
        db.insert(Listpcs.spliceData(tableNamePcsSet));
        db.update(19, 0, "pcsset");
    }

    if (pcsWrite == 1) {
        for (const auto& m : kSetMaps) {
            const double hmiValue = db.select(m.dbAddr, tableNamePcsSet);
            if (m.scale == 1) {
                himsmartenSetData.*(m.field) = static_cast<uint16_t>(hmiValue);
            } else {
                himsmartenSetData.*(m.field) =
                    static_cast<uint16_t>(std::lround(hmiValue * m.scale));
            }
        }
        himsmartenSetData.antiBackflowProtection_41696_bit1 =
            db.select(15, tableNamePcsSet) != 0;
        himsmartenSetData.powerFactorControl_41696_bit2 =
            db.select(16, tableNamePcsSet) != 0;
        himsmartenSetData.threePhaseUnbalancedMode_41696_bit3 =
            db.select(17, tableNamePcsSet) != 0;

        for (const auto& m : kSetMaps) {
            const uint16_t desired = himsmartenSetData.*(m.field);
            if ((smartenSetData.*(m.field)) != desired) {
                modbusclient.writeRegister(m.regAddr, desired, tableNamePcsSet);
            }
        }

        const uint16_t current41696 = pack41696ControlWord(
            smartenSetData.antiBackflowProtection_41696_bit1,
            smartenSetData.powerFactorControl_41696_bit2,
            smartenSetData.threePhaseUnbalancedMode_41696_bit3);
        const uint16_t desired41696 = pack41696ControlWord(
            himsmartenSetData.antiBackflowProtection_41696_bit1,
            himsmartenSetData.powerFactorControl_41696_bit2,
            himsmartenSetData.threePhaseUnbalancedMode_41696_bit3);
        if (current41696 != desired41696) {
            modbusclient.writeRegister(kReg41696, desired41696, tableNamePcsSet);
        }
        db.update(20, 0, "pcsset");
    }
}

void PCS_Smarten::writeLogicData(MySQLDatabase& db)
{
    db.update(101, static_cast<double>(smartenData.commFlag), tableNameLogic);
    db.update(102, static_cast<double>(smartenData.runState), tableNameLogic);

    const double activePower =
        static_cast<double>(smartenData.acBusTotalActivePower_40164) * pcsRatio_value1;
    db.update(103, activePower, tableNameLogic);

    db.update(105, static_cast<double>(smartenData.gridState), tableNameLogic);

    const double syncDone =
        smartenData.synchronizationDoneState_41727_bit3 ? 1.0 : 0.0;
    db.update(106, syncDone, tableNameLogic);
}

//更新PCS数据到数据库
void PCS_Smarten::updatePcsData(MySQLDatabase& db)
{
    Listpcs.clearData();
    for (const auto& field : kPcsFieldMaps) {
        Listpcs.addData(field.dbAddr, field.getter(smartenData));
    }
    db.insert(Listpcs.spliceData(tableNamePcs));
    writeLogicData(db);
}
//更新PCS历史数据
void PCS_Smarten::updatePcsHistoryData()
{
    listpcsHistory.clearData();
    for (const auto& field : kPcsFieldMaps) {
        listpcsHistory.addData(field.influxKey, field.getter(smartenData));
    }
    pcsInfluxDb.insert(listpcsHistory.spliceData(tableNamePcs));
}
//PCS线程
void PCS_Smarten::runPcsThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cyclePeriod = std::chrono::milliseconds(Config::PCS_JUDGMENT_DELAY);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            readRegisterData(pool);
        } catch (const std::exception& e) {
            std::cerr << "PCS线程异常: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "PCS线程未知异常" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}

bool PCS_Smarten::readHoldingBlock(int startAddr, int count)
{
    if (count < 1 || count > static_cast<int>(arr_holding_registers.size())) {
        return false;
    }
    if (modbusclient.readRegisters(startAddr, count, arr_holding_registers.data()) == -1) {
        return false;
    }
    return true;
}

uint32_t PCS_Smarten::decodeUInt32ByOffset(int offset) const
{
    if (offset < 0 || offset + 1 >= static_cast<int>(arr_holding_registers.size())) {
        return 0;
    }
    return (static_cast<uint32_t>(arr_holding_registers[offset]) << 16) |
           static_cast<uint32_t>(arr_holding_registers[offset + 1]);
}

uint64_t PCS_Smarten::decodeUInt64ByOffset(int offset) const
{
    if (offset < 0 || offset + 3 >= static_cast<int>(arr_holding_registers.size())) {
        return 0;
    }
    return (static_cast<uint64_t>(arr_holding_registers[offset]) << 48) |
           (static_cast<uint64_t>(arr_holding_registers[offset + 1]) << 32) |
           (static_cast<uint64_t>(arr_holding_registers[offset + 2]) << 16) |
           static_cast<uint64_t>(arr_holding_registers[offset + 3]);
}

