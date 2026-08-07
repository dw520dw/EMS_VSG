#ifndef SUNPV_H
#define SUNPV_H

#include <array>
#include <chrono>
#include <cstdint>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"
#include "logger.h"

// 逆变器工作状态（与设备字面值一致，可与 uint16_t 寄存器值比较）
enum class SunPvInverterState : uint16_t {
    Running = 0x0000,              // 运行：MPP 跟踪、DC/AC 正常输出
    Stop = 0x8000,                 // 停止：逆变器已停机
    ButtonStop = 0x1300,           // 按键停止：应用手动停 DSP，需应用手动启动恢复
    EmergencyStop = 0x1500,        // 紧急停止：干接点触发（需硬件干接点）
    Standby = 0x1400,              // 待机：直流输入不足，待机等待
    InitialStandby = 0x1200,       // 初始待机：上电初始待机
    Startup = 0x1600,              // 启动：初始化并与电网同步
    AlarmRunning = 0x9100,         // 报警运行：有告警信息仍运行
    DeratingRunning = 0x8100,      // 降额运行：温度/海拔等导致主动降额
    SchedulingRunning = 0x8200,  // 调度运行：按监控后台调度指令运行
    Fault = 0x5500,                // 故障：停机并断交流继电器，排除后可自恢复
    CommunicationFault = 0x2500, // 通信故障：ARM 与 DSP 通信失败
    Uninitialized = 0x1111,        // 未初始化
};


struct STR_SUNPV_Data {
    uint16_t DailyGeneration;     //日发电量
    uint32_t MonthlyGeneration;   //月发电量（5114 区双字）
    uint32_t TotalGeneration;     //总发电量
    uint32_t RunningTime;         //运行时间
    int16_t AmbientTemperature;   //内部温度
    uint16_t Mppt1Voltage;        //MPPT1电压
    uint16_t Mppt1Current;        //MPPT1电流
    uint16_t Mppt2Voltage;        //MPPT2电压
    uint16_t Mppt2Current;        //MPPT2电流
    uint16_t Mppt3Voltage;        //MPPT3电压
    uint16_t Mppt3Current;        //MPPT3电流
    uint16_t Mppt4Voltage;        //MPPT4电压
    uint16_t Mppt4Current;        //MPPT4电流
    uint16_t Mppt5Voltage;        //MPPT5电压
    uint16_t Mppt5Current;        //MPPT5电流
    uint16_t Mppt6Voltage;        //MPPT6电压
    uint16_t Mppt6Current;        //MPPT6电流
    uint16_t Mppt7Voltage;        //MPPT7电压
    uint16_t Mppt7Current;        //MPPT7电流
    uint16_t Mppt8Voltage;        //MPPT8电压
    uint16_t Mppt8Current;        //MPPT8电流
    uint16_t Mppt9Voltage;        //MPPT9电压
    uint16_t Mppt9Current;        //MPPT9电流
    uint16_t Mppt10Voltage;        //MPPT10电压
    uint16_t Mppt10Current;        //MPPT10电流
    uint16_t Mppt11Voltage;        //MPPT11电压
    uint16_t Mppt11Current;        //MPPT11电流
    uint16_t Mppt12Voltage;        //MPPT12电压
    uint16_t Mppt12Current;        //MPPT12电流
    uint32_t TotalApparentPower;  //总视在功率
    uint32_t TotalActivePower;   //总有功功率
    uint32_t TotalReactivePower;  //总无功功率
    int16_t TotalPowerFactor;    //总功率因数
    uint32_t DCTotalPower;    //直流总功率
    uint16_t APhaseVoltage;       //A相电压
    uint16_t BPhaseVoltage;       //B相电压
    uint16_t CPhaseVoltage;       //C相电压
    uint16_t APhaseCurrent;       //A相电流
    uint16_t BPhaseCurrent;       //B相电流
    uint16_t CPhaseCurrent;       //C相电流
    uint16_t Frequency;           //频率
    uint16_t MPPTStatus;          //MPPT状态
};

// 5044 为单个输入寄存器，同一时刻只有一个 uint16_t 故障码；解析后至多一个语义项为 true（未收录码则全 false）
struct STR_SUNPV_Alarm5044 {
    bool GridOvervoltage = false;           // 2,3,14,15 电网过电压
    bool GridUndervoltage = false;          // 4,5 电网欠电压
    bool GridOverfrequency = false;         // 8 电网过频
    bool GridUnderfrequency = false;        // 9 电网欠频
    bool GridPowerOutage = false;           // 10 电网停电
    bool LeakageCurrentHigh = false;        // 12 漏电流过大
    bool GridAbnormality = false;           // 13 电网异常
    bool GridVoltageUnbalance = false;      // 17 电网电压不平衡
    bool PvBackupConnectionFault = false;   // 28,29,208,448–479 光伏备用连接故障
    bool PvReverseConnectionAlarm = false;  // 532–547,564–579 光伏反向连接报警
    bool PvAbnormalityAlarm = false;        // 548–563,580–595 光伏异常报警
    bool MpptReverseConnection = false;     // 264–283 MPPT反向连接
    bool BoostCapOvervoltageAlarm = false;  // 332–363 升压电容器过压报警
    bool BoostCapOvervoltageFault = false;  // 364–395 升压电容器过压故障
    bool PvCableShortToGround = false;      // 1328 光伏电缆对地短路
    bool StringCurrentBackflow = false;     // 1548–1579 字符串电流回流
    bool PvGroundingFault = false;          // 1600–1611 光伏接地故障
    bool SystemHardwareFault = false;       // 1616 系统硬件故障
    bool AmbientTempHigh = false;           // 37 环境温度过高
    bool AmbientTempLow = false;            // 43 环境温度过低
    bool InsulationResistanceLow = false;   // 39 系统绝缘电阻过低
    bool GroundCableFault = false;          // 106 接地电缆故障
    bool ArcFault = false;                  // 88 电弧故障
    bool MeterCtReverseAlarm = false;       // 84 电表/CT反接报警
    bool MeterCommAbnormalityAlarm = false; // 514 电表通信异常报警
    bool GridConflict = false;              // 323 电网冲突
    bool InverterGridTiedCommAlarm = false; // 75 逆变器并网通信报警
    bool SystemFault = false;               // 手册「系统故障」大类（见实现内区间表）
    bool SystemAlarm = false;               // 手册「系统警报」大类
};

struct STR_SUNPV_Alarm5151 {
    bool PIDImpedanceAbnormal = false;     //PID阻抗异常
    bool PIDFunctionAbnormal = false;        //PID功能异常
    bool PIDVoltageCurrentProtection = false;        //PID电压/电流保护 
};


struct STR_SUNPV_SetData {
    uint16_t ActivePowerLimitSwitch = 0; //有功功率限制开关
    uint16_t ActivePowerLimit = 0; //有功功率限制
    uint16_t NumberOfModules = 0; //模块数量
    uint16_t RatedPower = 0; //额定功率
    uint16_t ReactivePowerLimitSwitch = 0; //无功功率限制开关
    uint16_t ReactivePowerLimit = 0; //无功功率限制
    uint16_t PowerFactorLimitSwitch = 0; //功率因数限制开关
    int16_t PowerFactorLimit = 0; //功率因数限制
};

const double sunPvRatio_value1 = 0.1;
const double sunPvRatio_value2 = 0.01;
const double sunPvRatio_value3 = 0.001;
// 5044 单字单码：先清零 out，按手册优先级命中一类即 return（与寄存器仅一个值一致）
void sunPvDecodeAlarm5044(uint16_t code5044, STR_SUNPV_Alarm5044& out);

class SunPv {
public:
    static constexpr std::size_t kSunPvMaxInverters = 8; // Modbus 从站台数上限
    // com_alarm：4 台光伏逆变器 amount 1~4 对应 addr 12~15
    static constexpr int kComAlarmSunPvCommAddrBase = 12;

    SunPv(const char* serial_port, int baud_rate);
    ~SunPv();
    void runSunPvThread(MySQLConnectionPool& pool);
    
private:
    bool readAndWriteData(MySQLConnectionPool& pool, int slaveID, int amount);
    void writeDataToDatabase(MySQLConnectionPool& pool, int amount);
    void writeDataHistory(int amount);

    ModbusRTU modbusclient;
    std::array<STR_SUNPV_Data, kSunPvMaxInverters> sunPv_data{};
    std::array<STR_SUNPV_Alarm5044, kSunPvMaxInverters> sunPv_alarm5044{};
    std::array<STR_SUNPV_Alarm5151, kSunPvMaxInverters> sunPv_alarm5151{};
    std::array<STR_SUNPV_SetData, kSunPvMaxInverters> sunPv_setData{};
    STR_SUNPV_SetData sunPv_himSetData;
    std::array<int, kSunPvMaxInverters> commErrCount{}; // 下标 amount-1
    std::array<int, kSunPvMaxInverters> commErrflg{};
    std::array<std::chrono::steady_clock::time_point, kSunPvMaxInverters> lastHistoryTime{};
    uint16_t arr_uint16[50] = {0};
    std::string tableNameSunPv = "sunpv";
    databaseList List;
    DatabaseList infList;
    influxDB influxDb;
};




#endif // SUNPV_H
