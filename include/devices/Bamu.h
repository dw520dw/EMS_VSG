#ifndef BAMU_H
#define BAMU_H

#include <array>
#include <chrono>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"

// 内联辅助函数：从位数组中提取指定位并设置标志
inline void setBitFromArray(bool& flag, const uint8_t* bitArray, int bitIndex) {
    flag = (bitArray[bitIndex] != 0);
}

// 倍率常量
const double  BamuRatio_Value = 0.1;    // 电压/电流倍率
const double  BamuRatio_Value3 = 0.001; // 单体电压倍率

// BMS 单体数量（与 Modbus 点表、写库 addr 一致）
static constexpr int BMS_CELL_VOLTAGE_COUNT = 260;     
static constexpr int BMS_CELL_TEMPERATURE_COUNT = 135; 
static constexpr int BMS_CELL_MODBUS_BLOCK = 120;   // 电压/温度第一段连续读长度
static constexpr int BMS_CELL_MODBUS_BLOCK1 = 15; // 温度第二段读 15 个（120+15=135）
static constexpr int BMS_CELL_MODBUS_BLOCK2 = 20; // 电压第三段读 20 个（120+120+20=260）
const int MAX_REGISTER_COUNT = 128;     // 输入寄存器缓冲（单次最大读 120）

// Modbus地址常量
const int BAMU_DATA_START_ADDR = 2;     // BAMU数据起始地址
const int BAMU_DATA_COUNT = 46;         // BAMU数据寄存器数量
const int BAMU_ALARM_START_ADDR = 1;    // BAMU报警起始地址
const int BAMU_ALARM_COUNT = 80;        // BAMU报警位数量
const int DI_START_ADDR = 0x898;        // DI起始地址
const int DI_COUNT = 13;                // DI数量
const int AIR_DATA_START_ADDR = 60100;    // 空调过程数据起始地址（每单元 +100）
const int AIR_DATA_COUNT = 43;            // 60100~60142 一次读完（中间有空洞寄存器）
const int AIR_ALARM_START_ADDR = 2213;    // 空调告警离散输入起始地址
const int AIR_ALARM_COUNT = 93;           // 空调告警位数量（2213~2305）
const int AIR_COMM_MYSQL_ADDR = 0;          // 空调通信故障（离散量 2213，写入数据区）
const int AIR_DATA_MYSQL_START = 1;       // 空调过程数据起始 addr
const int AIR_ALARM_MYSQL_START = 17;     // 空调告警起始 addr（0~16 数据区，不含通信故障）
const int AIR_ALARM_MYSQL_COUNT = AIR_ALARM_COUNT - 1; // 告警 92 项（不含通信故障）
const int MAX_INPUT_BITS = AIR_ALARM_COUNT; // 离散输入缓冲，与空调告警位数一致

// 本机 BMS 簇数量上限（与 qt708 配置、MySQL 表 bms1~bms10 一致）
static constexpr int BAMU_MAX_BMS_CLUSTER = 10;
// 每 2 簇 BMS 共用 1 台空调（air1 对应簇 1~2，air2 对应簇 3~4 …）
static constexpr int BMS_CLUSTERS_PER_AIR_UNIT = 2;
static constexpr int BAMU_MAX_AIR_UNIT = (BAMU_MAX_BMS_CLUSTER + 1) / 2;
// com_alarm：空调单元 1~5 对应 addr 7~11（与 BAMU addr1、BMS addr2 等错开）
static constexpr int AIR_COM_ALARM_ADDR_BASE = 7;

inline bool isValidBmsClusterId(int id) {
    return id >= 1 && id <= BAMU_MAX_BMS_CLUSTER;
}

inline int clampBmsClusterCount(int count) {
    if (count < 1) {
        return 1;
    }
    if (count > BAMU_MAX_BMS_CLUSTER) {
        return BAMU_MAX_BMS_CLUSTER;
    }
    return count;
}

inline bool isValidAirUnitId(int airUnitId) {
    return airUnitId >= 1 && airUnitId <= BAMU_MAX_AIR_UNIT;
}

/** BMS 簇号 -> 空调单元号：簇 1~2->air1，3~4->air2 … */
inline int bmsClusterToAirUnitId(int bmsClusterId) {
    return (bmsClusterId + BMS_CLUSTERS_PER_AIR_UNIT - 1) / BMS_CLUSTERS_PER_AIR_UNIT;
}

/** 是否为该空调单元的首簇（负责 Modbus 读取、写库与空调历史上传） */
inline bool isAirUnitLeadCluster(int bmsClusterId) {
    return (bmsClusterId - 1) % BMS_CLUSTERS_PER_AIR_UNIT == 0;
}

/** 空调单元号 -> com_alarm 地址（air1=7 … air5=11） */
inline int airUnitToComAlarmAddr(int airUnitId) {
    return AIR_COM_ALARM_ADDR_BASE + airUnitId - 1;
}

/** 空调单元号 -> logic 表通讯故障地址（air1=201，air2=1201 …） */
inline int airUnitToLogicCommAddr(int airUnitId) {
    return 201 + (airUnitId - 1) * 1000;
}

struct BAMUData { 
    uint16_t Voltage;//电压
    int16_t Current; //电流
    int32_t Power;//功率
    uint16_t SOC;//SOC
    uint16_t SOH;//SOH
    uint16_t Max_Cell_Voltage;//最高单体电压
    uint16_t Max_Cell_Voltage_group;//最高单体电压组号
    uint16_t Max_Cell_Voltage_Number;//最高单体电压单体号
    uint16_t Min_Cell_Voltage;//最低单体电压
    uint16_t Min_Cell_Voltage_group;//最低单体电压组号
    uint16_t Min_Cell_Voltage_Number;//最低单体电压单体号
    uint16_t Max_Cell_Temperature;//最高单体温度
    uint16_t Max_Cell_Temperature_group;//最高单体温度组号
    uint16_t Max_Cell_Temperature_Number;//最高单体温度单体号
    uint16_t Min_Cell_Temperature;//最低单体温度
    uint16_t Min_Cell_Temperature_group;//最低单体温度组号
    uint16_t Min_Cell_Temperature_Number;//最低单体温度单体号
    uint32_t Charging_Capacity;//充电容量
    uint32_t Discharging_Capacity;//放电容量
    uint32_t Single_Charging_Capacity;//单次充电容量
    uint32_t Single_Discharging_Capacity;//单次放电容量
    uint32_t Day_Charging_Capacity;//日充电容量
    uint32_t Day_Discharging_Capacity;//日放电容量
    uint16_t Allow_Max_Charging_Power;//允许最大充电功率
    uint16_t Allow_Max_Discharging_Power;//允许最大放电功率
    uint16_t Allow_Max_Charging_Current;//允许最大充电电流
    uint16_t Allow_Max_Discharging_Current;//允许最大放电电流
    uint16_t Run_Temperture;//运行温度
    uint16_t State;//状态
    uint16_t Insulation_Resistance;//绝缘电阻
    uint16_t commErrCount;//通信错误计数
    int commFlag; // 0:OK, 1:NG
    int alarmlevel; // 1-2-3
};
struct BAMUAlarm { 
    bool mBankVolLower_Lv1;//电池组电压轻微下限报警
    bool mBankVolLower_Lv2;//电池组电压一般下限报警
    bool mBankVolLower_Lv3;///电池组电压严重下限报警
    bool mBankVolUpper_Lv1;//电池组电压轻微上限报警
    bool mBankVolUpper_Lv2;//电池组电压一般上限报警
    bool mBankVolUpper_Lv3;//电池组电压严重上限报警
    bool mBankCurrentUpper_Lv1;//电池组电流轻微上限报警
    bool mBankCurrentUpper_Lv2;//电池组电流一般上限报警
    bool mBankCurrentUpper_Lv3;//电池组电流严重上限报警
    bool mmInsLower_Lv1;//绝缘电阻轻微下限报警
    bool mmInsLower_Lv2;//绝缘电阻一般下限报警
    bool mmInsLower_Lv3;//绝缘电阻严重下限报警
    bool mModelTempLower_Lv1;//模块温度轻微下限报警
    bool mModelTempLower_Lv2;//模块温度一般下限报警
    bool mModelTempLower_Lv3;//模块温度严重下限报警
    bool mModelTempUpper_Lv1;//模块温度轻微上限报警
    bool mModelTempUpper_Lv2;//模块温度一般上限报警
    bool mModelTempUpper_Lv3;//模块温度严重上限报警
    bool mCellVolUpper_Lv1;//单体电压轻微上限报警
    bool mCellVolUpper_Lv2;//单体电压一般上限报警
    bool mCellVolUpper_Lv3;//单体电压严重上限报警
    bool mCellVolLower_Lv1;//单体电压轻微下限报警
    bool mCellVolLower_Lv2;//单体电压一般下限报警
    bool mCellVolLower_Lv3;//单体电压严重下限报警
    bool mCellVolDif_Lv1;//单体电压差轻微报警
    bool mCellVolDif_Lv2;//单体电压差一般报警
    bool mCellVolDif_Lv3;//单体电压差严重报警
    bool mCellTempLower_Lv1;//单体温度轻微下限报警
    bool mCellTempLower_Lv2;//单体温度一般下限报警
    bool mCellTempLower_Lv3;//单体温度严重下限报警
    bool mCellTempUpper_Lv1;//单体温度轻微上限报警
    bool mCellTempUpper_Lv2;//单体温度一般上限报警
    bool mCellTempUpper_Lv3;//单体温度严重上限报警
    bool mCellTempDif_Lv1;//单体温度差轻微报警
    bool mCellTempDif_Lv2;//单体温度差一般报警
    bool mCellTempDif_Lv3;//单体温度差严重报警
    bool SOCLower_Lv1;//SOC轻微下限报警
    bool SOCLower_Lv2;//SOC一般下限报警
    bool SOCLower_Lv3;//SOC严重下限报警
    bool SOCUpper_Lv1;//SOC轻微上限报警
    bool SOCUpper_Lv2;//SOC一般上限报警
    bool SOCUpper_Lv3;//SOC严重上限报警
    bool SOHLower_Lv1;//SOH轻微下限报警
    bool SOHLower_Lv2;//SOH一般下限报警
    bool SOHLower_Lv3;//SOH严重下限报警
    bool SOHUpper_Lv1;//SOH轻微上限报警
    bool SOHUpper_Lv2;//SOH一般上限报警
    bool SOHUpper_Lv3;//SOH严重上限报警
    bool MainCtrl_Disconnected;//主控断开报警
    bool SBC_Disconnected;//从控断开报警
    bool Voltage_Abnormal_Alarm;//电压异常报警
    bool Contactor_Break_Alarm;//接触器断开异常报警
    bool Contactor_Close_Alarm;//接触器闭合异常报警
    bool Charging_stop_Alarm;//充电停止报警
    bool Discharging_stop_Alarm;//放电停止报警
    bool BMS_Alarm_Total;//BMS报警汇总
    bool BMS_Fault_Total;//BMS故障汇总
    bool mBankTempUpper_Lv1;//电池组温度轻微上限报警
    bool mBankTempUpper_Lv2;//电池组温度一般上限报警
    bool mBankTempUpper_Lv3;//电池组温度严重上限报警
    bool mModelVolUpper_Lv1;//模块电压轻微上限报警
    bool mModelVolUpper_Lv2;//模块电压一般上限报警
    bool mModelVolUpper_Lv3;//模块电压严重上限报警
    bool mModelVolLower_Lv1;//模块电压轻微下限报警
    bool mModelVolLower_Lv2;//模块电压一般下限报警
    bool mModelVolLower_Lv3;//模块电压严重下限报警
    bool mCellVolAcqFault;//单体电压采集故障
    bool mCellTempAcqFault;//单体温度采集故障
};

struct BMSData {
    uint16_t Battery_State;//电池状态
    uint16_t Allow_Max_Charging_Power;//允许最大充电功率
    uint16_t Allow_Max_Discharging_Power;//允许最大放电功率
    uint16_t Allow_Max_Charging_Voltage;//允许最大充电电压
    uint16_t Allow_Max_Discharging_Voltage;//允许最大放电电压
    uint16_t Allow_Max_Charging_Current;//允许最大充电电流
    uint16_t Allow_Max_Discharging_Current;//允许最大放电电流
    uint16_t DI1;//主正接触器反馈
    uint16_t DI2;//熔断器反馈
    uint16_t DI3;//主负接触器反馈
    uint16_t DI4;//预留
    uint16_t DI5;//主控编址输入
    uint16_t DI6;//断路器反馈
    uint16_t DI7;//预留
    uint16_t DI8;//预留
    uint16_t Voltage;//电压
    int16_t Current;
    int32_t Power;//功率
    uint16_t Module_Temperature;//模块温度
    uint16_t SOC;//SOC
    uint16_t SOH;//SOH
    uint16_t Insulation_Resistance;
    uint16_t Ave_Cell_Voltage;//平均单体电压
    uint16_t Ave_Cell_Temperature;//最高单体温度
    uint16_t Max_Cell_Voltage;//最高单体电压
    uint16_t Max_Cell_Voltage_Number;//最高单体电压单体号
    uint16_t Min_Cell_Voltage;//最低单体电压
    uint16_t Min_Cell_Voltage_Number;//最低单体电压单体号
    uint16_t Max_Cell_Temperature;//最高单体温度
    uint16_t Max_Cell_Temperature_Number;//最高单体温度单体号
    uint16_t Min_Cell_Temperature;//最低单体温度
    uint16_t Min_Cell_Temperature_Number;//最低单体温度单体号
    uint16_t Cell_Voltage[BMS_CELL_VOLTAGE_COUNT];       // 单体电压
    uint16_t Cell_Temperature[BMS_CELL_TEMPERATURE_COUNT]; // 单体温度
    uint16_t commErrCount;//通信错误计数
    int commFlag; // 0:OK, 1:NG

};

struct BMSAlarm
{ 
    bool Comm_Error;//通信错误
    bool mBankVolLower_Lv1;//电池组电压轻微下限报警
    bool mBankVolLower_Lv2;//电池组电压一般下限报警
    bool mBankVolLower_Lv3;///电池组电压严重下限报警
    bool mBankVolUpper_Lv1;//电池组电压轻微上限报警
    bool mBankVolUpper_Lv2;//电池组电压一般上限报警
    bool mBankVolUpper_Lv3;//电池组电压严重上限报警
    bool mBankCurrentUpper_Lv1;//电池组电流轻微上限报警
    bool mBankCurrentUpper_Lv2;//电池组电流一般上限报警
    bool mBankCurrentUpper_Lv3;//电池组电流严重上限报警
    bool mCellVolLower_Lv1;//单体电压轻微下限报警
    bool mCellVolLower_Lv2;//单体电压一般下限报警
    bool mCellVolLower_Lv3;//单体电压严重下限报警
    bool mCellVolUpper_Lv1;//单体电压轻微上限报警
    bool mCellVolUpper_Lv2;//单体电压一般上限报警
    bool mCellVolUpper_Lv3;//单体电压严重上限报警
    bool mCellTempLower_Lv1;//单体温度轻微下限报警
    bool mCellTempLower_Lv2;//单体温度一般下限报警
    bool mCellTempLower_Lv3;//单体温度严重下限报警
    bool mCellTempUpper_Lv1;//单体温度轻微上限报警
    bool mCellTempUpper_Lv2;//单体温度一般上限报警
    bool mCellTempUpper_Lv3;//单体温度严重上限报警
    bool mmInsLower_Lv1;//绝缘电阻轻微下限报警
    bool mmInsLower_Lv2;//绝缘电阻一般下限报警
    bool mmInsLower_Lv3;//绝缘电阻严重下限报警
    bool SOCLower_Lv1;//SOC轻微下限报警
    bool SOCLower_Lv2;//SOC一般下限报警
    bool SOCLower_Lv3;//SOC严重下限报警
    bool SOCUpper_Lv1;//SOC轻微上限报警
    bool SOCUpper_Lv2;//SOC一般上限报警
    bool SOCUpper_Lv3;//SOC严重上限报警
    bool SOHLower_Lv1;//SOH轻微下限报警
    bool SOHLower_Lv2;//SOH一般下限报警
    bool SOHLower_Lv3;//SOH严重下限报警
    bool mCellVolDif_Lv1;//单体电压差轻微报警
    bool mCellVolDif_Lv2;//单体电压差一般报警
    bool mCellVolDif_Lv3;//单体电压差严重报警
    bool mCellTempDif_Lv1;//单体温度差轻微报警
    bool mCellTempDif_Lv2;//单体温度差一般报警
    bool mCellTempDif_Lv3;//单体温度差严重报警
    bool mModelTempUpper_Lv1;//模块温度轻微上限报警
    bool mModelTempUpper_Lv2;//模块温度一般上限报警
    bool mModelTempUpper_Lv3;//模块温度严重上限报警
    bool mModelVolUpper_Lv1;//模块电压轻微上限报警
    bool mModelVolUpper_Lv2;//模块电压一般上限报警
    bool mModelVolUpper_Lv3;//模块电压严重上限报警
    bool mModelVolLower_Lv1;//模块电压轻微下限报警
    bool mModelVolLower_Lv2;//模块电压一般下限报警
    bool mModelVolLower_Lv3;//模块电压严重下限报警
    bool mCellVolAcqFault;//单体电压采集故障
    bool mCellTempAcqFault;//单体温度采集故障
};

struct AirData
{
    uint16_t unit_status;                    // 空调整机状态
    uint16_t operation_mode;                 // 运行模式
    uint16_t fan_status;                     // 风机状态
    uint16_t fan_speed;                      // 风机转速
    uint16_t water_pump_status;              // 水泵状态
    uint16_t water_pump_speed;               // 水泵转速
    uint16_t compressor_status;              // 压缩机状态
    uint16_t compressor_speed;               // 压缩机转速
    int16_t cooling_setpoint;                // 制冷点
    int16_t heating_setpoint;                // 加热点
    int16_t cooling_hysteresis;              // 制冷回差
    int16_t heating_hysteresis;              // 加热回差
    int16_t outlet_water_temperature;        // 出水温度
    int16_t return_water_temperature;        // 回水温度
    int16_t inlet_temperature;               // 进气温度
    int16_t exhaust_temperature;             // 排气温度
    int16_t ambient_temperature;             // 环境温度
    int16_t outlet_water_pressure;           // 出水压力
    int16_t return_water_pressure;           // 回水压力
};

struct AirAlarm
{
    bool comm_failure = false;                      // 通信故障 (2213)
    bool pump_idle_protection = false;              // 泵空转保护
    bool water_supply_pump_fault = false;           // 供水泵故障
    bool liquid_temp_probe_fault = false;           // 液温探头故障
    bool return_liquid_probe_fault = false;         // 回液探头故障
    bool evap_inlet_probe_fault = false;            // 蒸发进口探头故障
    bool evap_outlet_probe_fault = false;           // 蒸发出口探头故障
    bool supply_pressure_sensor_fault = false;      // 供水压力传感器故障
    bool return_water_pressure_low = false;         // 回水压力低故障
    bool supply_pressure_too_high = false;          // 供水压力过高
    bool liquid_temp_too_high = false;              // 液温过高
    bool exhaust_pressure_sensor_fault = false;     // 排气压力传感器故障
    bool system_low_pressure = false;               // 系统低压
    bool fan_overload = false;                      // 风机过载
    bool screen_mainboard_comm_timeout = false;     // 屏幕与主板通讯超时
    bool compressor_inverter_comm_fault = false;    // 压缩机变频器通讯故障
    bool pump_overcurrent = false;                  // 泵过流故障
    bool pump_drive_overheat = false;               // 泵驱动过热保护故障
    bool pump_overvoltage = false;                    // 泵过压故障
    bool pump_undervoltage = false;                 // 泵欠压故障
    bool pump_phase_loss = false;                   // 泵缺相
    bool return_pressure_sensor_fault = false;      // 回水压力传感器故障
    bool power_loss_alarm = false;                  // 掉电报警
    bool supply_pressure_high_alarm = false;        // 供水压力过高报警
    bool electric_heating_protection = false;       // 电加热保护
    bool voltage_too_high = false;                  // 电压过高
    bool system_high_pressure_fault = false;        // 系统高压故障
    bool system_high_pressure_lock = false;         // 系统高压报警锁定
    bool motherboard_ttc_fault = false;             // 主板TTC故障
    bool slave_board_comm_fault = false;            // 从板通讯故障
    bool ambient_probe_fault = false;               // 环境探头故障
    bool pump_stall_protection = false;             // 泵堵转保护
    bool voltage_too_low = false;                   // 电压过低
    bool liquid_temp_too_low = false;               // 液温过低
    bool compressor_start_instant_oc = false;       // 压机启动瞬间过流
    bool compressor_accel_oc = false;               // 压机加速运行过流
    bool compressor_decel_oc = false;               // 压机减速运行过流
    bool compressor_constant_oc = false;            // 压机恒速运行过流
    bool compressor_accel_ov = false;               // 压机加速运行过压
    bool compressor_decel_ov = false;               // 压机减速运行过压
    bool compressor_constant_ov = false;            // 压机恒速运行过压
    bool compressor_standby_ov = false;             // 压机待机时过压
    bool compressor_run_uv = false;                 // 压机运行中欠压
    bool compressor_output_phase_loss = false;      // 压机输出缺相
    bool compressor_power_device_prot = false;      // 压机功率器件保护
    bool compressor_overheat = false;               // 压机过热
    bool compressor_overload = false;               // 压机过载
    bool compressor_detects_overload = false;       // 压机检测压机过载
    bool compressor_load_heavy = false;             // 压机负载过重
    bool compressor_speed_high = false;             // 压机速度过大
    bool compressor_d_axis_oc = false;              // 压机D轴电流过大
    bool compressor_q_axis_oc = false;              // 压机Q轴电流过大
    bool compressor_self_tune_fail = false;         // 压机参数自整定失败
    bool compressor_comm_abnormal = false;          // 压机通讯异常
    bool compressor_current_detect_fault = false;   // 压机电流检测故障
    bool compressor_start_motor_stall = false;      // 压机启动中电机堵转
    bool compressor_run_motor_stall = false;        // 压机运行中电机堵转
    bool compressor_stall_fault = false;            // 压机失速故障
    bool compressor_thermal_cutout_1 = false;       // 压机中断温断1
    bool compressor_thermal_cutout_2 = false;       // 压机中断温断2
    bool compressor_start_rotor_jitter = false;     // 压机启动中转子抖动过大
    bool compressor_run_rotor_jitter = false;       // 压机运行中转子抖动过大
    bool pfc_overcurrent = false;                   // PFC过流
    bool pfc_peak_overcurrent = false;                // PFC峰值电流过大
    bool pfc_rms_overcurrent = false;                 // PFC有效值电流过大
    bool fan_start_instant_oc = false;              // 风机启动瞬间过流
    bool fan_accel_oc = false;                        // 风机加速运行过流
    bool fan_decel_oc = false;                        // 风机减速运行过流
    bool fan_constant_oc = false;                     // 风机恒速运行过流
    bool fan_accel_ov = false;                        // 风机加速运行过压
    bool fan_decel_ov = false;                        // 风机减速运行过压
    bool fan_constant_ov = false;                     // 风机恒速运行过压
    bool fan_standby_ov = false;                      // 风机待机时过压
    bool fan_run_uv = false;                          // 风机运行中欠压
    bool fan_input_phase_loss = false;                // 风机输入缺相
    bool fan_output_phase_loss = false;               // 风机输出缺相
    bool fan_power_device_prot = false;               // 风机功率器件保护
    bool fan_overheat = false;                        // 风机过热
    bool inverter_overload = false;                   // 变频器过载
    bool fan_overload_vfd = false;                    // 风机过载(变频器)
    bool external_fault = false;                      // 外部故障
    bool motor_load_heavy = false;                    // 电机负载过重
    bool inverter_underload = false;                  // 变频器欠载
    bool fan_fault = false;                           // 风扇故障
    bool prohibit_time_reached = false;               // 禁止时间已到
    bool param_storage_failed = false;                // 参数存储失败
    bool comm_abnormal = false;                       // 通讯异常
    bool current_detect_fault = false;                // 电流检测故障
    bool poor_self_tuning = false;                    // 自整定不良
    bool analog_input_disconnect = false;             // 模拟输入掉线
    bool pg_wire_break = false;                       // PG断线
    bool thermistor_open = false;                     // 热敏电阻开路
    bool abnormal_shutdown = false;                   // 异常停机故障 (2305)
};

// DI 信号（从 DI 口读取，用于 use_data 等）
struct DiSignals {
    bool EPO = false;       // 急停信号
    bool FireLevel1 = false;     //消防一级
    bool FireControl = false;  // 消防动作
    bool TotalSwitch = false;  // 总开关
};

struct Air
{
    AirData airData;
    AirAlarm airAlarm;
};

class Bamu
{
public:
    Bamu(const char* device_ip,int device_port, int device_address);
    ~Bamu();
    // 历史数据上传（Influx：数据与告警同表）
    void bamuHistorydata();                 // BAMU -> bamu
    void airHistorydata(int airUnitId);     // 空调 -> airN（N 为空调单元号，非 BMS 簇号）
    void write_Bmshistorydata(int id);      // BMS -> bmsN
    void write_Cell_historydata(int id);    // 单体 -> bms_cellvol/tempN

    void readBamuData();//读取BAMU数据
    void readBamuAlarm();//读取BAMU报警
    void alarmLevel();//报警等级
    void readAirData(int bmsClusterId);//仅首簇读 Modbus，缓存到对应空调单元
    bool readDiSignals(DiSignals& outData);//读取DI信号
    void readBmsData(int id);
    void write_bms_data(MySQLDatabase& db, int id);//写入BMS数据（含告警）
    void write_cell_data(MySQLDatabase& db, int id);

    // 统一的读写操作接口
    void processAllBamuOperations(MySQLDatabase& db);    // BAMU 汇总 + logic
    void processAllBmsOperations(MySQLDatabase& db, int id);     // 单簇 BMS + 空调写库

    /** 线程入口：BAMU/BMS 数据处理循环（实时库 + 10s 节流历史库，供 main 创建线程调用） */
    void runBamuDataThread(MySQLConnectionPool& pool);

    // 数据库写入函数
    void write_Bamu_data(MySQLDatabase& db);//写入BAMU数据（含告警）
    void write_air_data(MySQLDatabase& db, int bmsClusterId);//按簇映射到 air 表（两簇共一台空调）
    void write_logic_data(MySQLDatabase& db);      // 写入 logic 表（BAMU 关键量 + DI）

    // 公有数据成员，供外部直接访问
    BAMUData bamuData;                      // BAMU数据
    BAMUAlarm bamuAlarm;                    // BAMU报警
    DiSignals diSignals;                     // DI信号
    Air air[BAMU_MAX_AIR_UNIT + 1];                               // 索引0不用，1~5 空调单元（每单元 2 簇 BMS）
    BMSData bmsData[BAMU_MAX_BMS_CLUSTER + 1];                     // 索引0不用，1~10 对应各簇 BMS
    BMSAlarm bmsAlarm[BAMU_MAX_BMS_CLUSTER + 1];                   // 索引0不用，1~10 对应各簇 BMS 告警
private:
    ModbusTCP modbusclient; // ModbusTCP对象
    databaseList dbList;        // 数据库列表
    DatabaseList historyDbList; // 历史数据库列表
    influxDB influxDb;          // InfluxDB对象

    int batteryNumber = 0;// BMS 簇数量（qt708）

    uint16_t arr_uint16[50] = {0};                          // 通信测试缓冲
    uint16_t arr_input_registers[MAX_REGISTER_COUNT] = {0}; // 输入寄存器数组
    uint8_t arr_input_bits[MAX_INPUT_BITS] = {0};           // 输入位数组
    std::string TableNameBamu = "bamu";
    std::string TableNameBms = "bms";
    std::string tableNameBMScellvol = "bms_cellvol";
    std::string tableNameBMScelltemp = "bms_celltemp";
    std::string TableNameAir = "air";
    std::string tableNameLogic = "logic";

    std::chrono::steady_clock::time_point lastBamuHistoryTime{};
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_BMS_CLUSTER + 1> lastBmsHistoryTime{};
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_AIR_UNIT + 1> lastAirHistoryTime{};
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_BMS_CLUSTER + 1> lastCellHistoryTime{};
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_BMS_CLUSTER + 1> lastCellRealtimeTime{};

    static constexpr int kBmsCellVolDbCount = 261;   // addr 1~260
    static constexpr int kBmsCellTempDbCount = 136;  // addr 1~135

    std::array<std::array<double, kBmsCellVolDbCount>, BAMU_MAX_BMS_CLUSTER + 1> lastCellVolDbValues_{};
    std::array<std::array<double, kBmsCellTempDbCount>, BAMU_MAX_BMS_CLUSTER + 1> lastCellTempDbValues_{};
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasLastCellVolDbSnapshot_{};
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasLastCellTempDbSnapshot_{};

    void readBmsCellData(int id);  // 单体电压/温度（4 段 Modbus，与写库同频）
};

#endif // BAMU_H

