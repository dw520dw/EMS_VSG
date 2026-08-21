
// PCS_Smarten 储能变流器（PCS）Modbus TCP 通信驱动头文件
// 功能：定义 Smarten PCS 设备通信驱动类的接口与数据结构，
//       包括参数设定结构体 SmartenSetData 和主类 PCS_Smarten。

#ifndef PCS_SMARTEN_H
#define PCS_SMARTEN_H

#include "ModbusPollEngine.h"   // Modbus 轮询引擎
#include "ModbusRtu.h"          // Modbus RTU/TCP 通信基类
#include "MySQLDB_1.h"          // MySQL 数据库操作
#include <cstdint>              // 固定宽度整数类型
#include <memory>               // 智能指针
#include <string>               // 字符串

// ========== SmartenSetData 参数结构体 ==========
// 封装 PCS 设备所有可读写参数，分为"设备当前值"（deviceSet_）和
// "HMI 目标值"（hmiSet_）两份副本，用于比较差异后按需下发。
// 寄存器地址后缀（如 _41378）对应 Modbus 寄存器地址，便于对照协议查阅。
struct SmartenSetData {
    uint16_t alarmReset_41378 = 0;                    // 告警复位寄存器值（写 1 触发复位）
    uint16_t pcsOnOff_41379 = 0;                      // PCS 开关机控制（0=关, 1=开）
    int16_t activePowerSetting_41546 = 0;             // 有功功率设定值（单位 0.1kW）
    int16_t reactivePowerSetting_41547 = 0;           // 无功功率设定值（单位 0.1kvar）
    uint16_t gridInterMode_41671 = 0;                 // 并离网模式（0=并网, 1=离网）
    uint16_t dcVoltageLowerLimit_41473 = 0;           // 直流电压下限（缩放系数 0.1）
    uint16_t constantVoltageChargeVoltage_41474 = 0;  // 恒压充电电压（缩放系数 0.1）
    uint16_t dcOutputVoltage_41475 = 0;               // 直流输出电压（缩放系数 0.1）
    uint16_t dischargeTerminationVoltage_41478 = 0;   // 放电终止电压（缩放系数 0.1）
    uint16_t chargeCutoffCurrent_41480 = 0;           // 充电截止电流（缩放系数 0.1）
    uint16_t batteryProtectionSoc_41489 = 0;          // 电池保护 SOC（缩放系数 0.1）
    uint16_t dischargeLimitSoc_41490 = 0;             // 放电限制 SOC（缩放系数 0.1）
    uint16_t chargeLimitSoc_41491 = 0;                // 充电限制 SOC（缩放系数 0.1）
    uint16_t vsgeEnable_41682 = 0;                    // VSGE（虚拟同步发电机）使能
    uint16_t vsgeControlMode_41687 = 0;               // VSGE 控制模式
    uint16_t acConnectType_41580 = 0;                 // 交流接入类型
    uint16_t threePhaseUnbalancedMode_41582 = 0;      // 三相不平衡模式
    uint16_t ratedVoltageLevel_41543 = 0;             // 额定电压等级
    uint16_t ratedFrequencyLevel_41544 = 0;           // 额定频率等级
    uint16_t energyControlMode_41701 = 0;             // 能量控制模式
    bool antiBackflowProtection_41696_bit1 = false;   // 防逆流保护使能（寄存器 41696 bit1）
    bool powerFactorControl_41696_bit2 = false;       // 功率因数控制使能（寄存器 41696 bit2）
    bool threePhaseUnbalancedMode_41696_bit3 = false; // 三相不平衡模式使能（寄存器 41696 bit3）
};

// ========== PCS_Smarten 类声明 ==========
// Smarten PCS 储能变流器 Modbus TCP 通信驱动类
// 负责：周期性轮询遥测数据、计算虚拟量、写入逻辑数据、下发 HMI 控制指令、
//       处理 PCS 参数批量读写（与 MySQL pcsset 表交互）。
class PCS_Smarten {
public:
    // 构造函数：加载 Modbus 设备配置，建立 TCP 连接，注册后解码钩子
    // 参数 configPath: 配置文件路径，为空时使用 Config 默认路径
    explicit PCS_Smarten(const std::string& configPath = std::string());

    // 析构函数：断开 Modbus TCP 连接，释放资源
    ~PCS_Smarten();

    // PCS 通信主循环线程入口
    // 参数 pool: MySQL 连接池引用
    void runPcsThread(MySQLConnectionPool& pool);

private:
    // 遥测后处理总入口：在每次成功解码遥测数据后被回调
    // 依次执行：填充虚拟量 → 写入逻辑数据 → 下发 HMI 控制指令 → 处理参数读写
    // 参数 eng: Modbus 轮询引擎引用
    // 参数 db:  MySQL 数据库操作对象
    void onAfterTelemetry(ModbusPollEngine& eng, MySQLDatabase& db);

    // 根据底层寄存器状态计算并填充虚拟量到轮询引擎
    // 计算的虚拟量：Status（综合运行状态）、OnGrid（并离网状态）、
    //   DC/AC 充放电量（64 位寄存器，缩放系数 0.1 → kWh）
    // 参数 eng: 轮询引擎引用
    void fillVirtual(ModbusPollEngine& eng);

    // 读取设备当前设定值与 HMI 目标值，比较后下发控制指令
    // 支持：PCS 开关机、并离网切换、告警复位、有功/无功功率设定（带死区）
    // 参数 db: 数据库操作对象（读取 HMI 指令、更新复位标志）
    void dispatchHmiControlCommands(MySQLDatabase& db);

    // 处理 PCS 参数的批量读取与写入（与数据库 pcsset 表交互）
    // 当 pcsRead_=1 时：从设备读取全部参数 → 写入数据库 → 清除读标志
    // 当 pcsWrite_=1 时：从数据库读取目标值 → 与设备当前值比较 → 差异项写入设备 → 清除写标志
    // 参数 db: 数据库操作对象
    void pcsSetData(MySQLDatabase& db);

    // 将关键逻辑数据写入 MySQL 数据库 logic 表
    // 写入：通信状态标志、综合运行状态、交流有功功率、并离网状态等
    // 参数 eng: 轮询引擎引用（获取通信标志和虚拟量）
    // 参数 db:  数据库操作对象
    void writeLogicData(ModbusPollEngine& eng, MySQLDatabase& db);

    // 读取 4 个连续寄存器拼接为 64 位无符号整数，并按比例缩放
    // 参数 startAddr: 起始寄存器地址（大端序，w[0]为最高位）
    // 参数 scale:     缩放系数
    // 返回: 缩放后的浮点数值；读取失败返回 0.0
    double readU64Scaled(int startAddr, double scale);

    // Modbus TCP 通信总线对象（智能指针管理）
    std::unique_ptr<ModbusTCP> bus_;

    // Modbus 轮询引擎（智能指针管理）
    std::unique_ptr<ModbusPollEngine> engine_;

    // 设备当前参数设定值（从设备读取）
    SmartenSetData deviceSet_{};

    // HMI 目标参数设定值（从数据库读取）
    SmartenSetData hmiSet_{};

    // PCS 参数数据列表（用于批量写入数据库）
    databaseList listPcs_;

    // Modbus 寄存器读取缓冲区（最多 32 个寄存器）
    uint16_t arr_[32]{};

    // 数据库 pcsset 表读请求标志（1=请求读取）
    int pcsRead_ = 0;

    // 数据库 pcsset 表写请求标志（1=请求写入）
    int pcsWrite_ = 0;
};

#endif  // PCS_SMARTEN_H