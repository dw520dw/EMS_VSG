
/**
 * @file Dido.h
 * @brief Dido（数字量输入输出）Modbus TCP 通信驱动头文件
 *
 * 本模块负责通过 Modbus TCP 协议与 Dido 设备进行交互，主要功能包括：
 * 1. 周期性轮询设备数字量输入状态（黄灯/绿灯/红灯/柴发开关/总开关反馈）；
 * 2. 接收 HMI 控制指令并下发至设备（黄灯/绿灯/红灯/柴发开关/总开关控制）；
 * 3. 将关键数字量状态写入 MySQL 数据库。
 */

#ifndef DIDO_H
#define DIDO_H

// ========== 系统头文件 ==========
#include "ModbusPollEngine.h"  // Modbus 轮询引擎，负责设备通信与数据解码
#include "ModbusRtu.h"         // Modbus TCP 通信总线实现
#include "MySQLDB_1.h"         // MySQL 数据库操作封装
#include <memory>              // std::unique_ptr 智能指针
#include <string>              // std::string 字符串

// ========== Dido 类定义 ==========

class Dido {
public:
    // 构造函数：加载 Modbus 设备配置、建立 TCP 连接、注册后解码钩子
    // @param configPath 配置文件路径，为空时使用默认配置
    explicit Dido(const std::string& configPath = std::string());

    // 析构函数：断开 Modbus TCP 连接，释放资源
    ~Dido();

    // Dido 主循环线程：以固定周期轮询设备数据
    // @param pool MySQL 连接池引用
    void didoThread(MySQLConnectionPool& pool);

private:
    // 遥测后处理总入口：在每次成功解码遥测数据后被回调
    // 功能：读取 HMI 控制指令 → 读取设备反馈 → 下发 DO 指令 → 写入虚拟量 → 更新数据库
    // @param eng 轮询引擎引用
    // @param db  数据库操作对象
    void onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db);

    // 比较目标值与当前值，仅不同时下写寄存器，避免无效写入
    // @param reg    目标寄存器地址
    // @param desired 目标布尔值
    // @param actual  设备当前实际值（引用，写入后同步更新）
    // @param name    日志名称标识
    void writeDoIfChanged(int reg, bool desired, bool& actual, const char* name);

    // ========== 成员变量说明 ==========

    std::unique_ptr<ModbusTCP> bus_;           // Modbus TCP 通信总线智能指针
    std::unique_ptr<ModbusPollEngine> engine_; // Modbus 轮询引擎智能指针

    // HMI 下发控制目标值（来自数据库 data_total 表）
    bool yellowLed_ = false;      // 黄灯控制目标值（寄存器 200）
    bool greenLed_ = false;       // 绿灯控制目标值（寄存器 201）
    bool redLed_ = false;         // 红灯控制目标值（寄存器 202）
    bool chiFaSwitch_ = false;    // 柴发开关控制目标值（寄存器 203）
    bool qacSplit_ = false;       // 总开关控制目标值（寄存器 204）

    // 设备端实际反馈值（从寄存器 200~204 读取）
    bool readYellow_ = false;     // 黄灯实际反馈值
    bool readGreen_ = false;      // 绿灯实际反馈值
    bool readRed_ = false;        // 红灯实际反馈值
    bool readChiFa_ = false;      // 柴发开关实际反馈值
    bool readQac_ = false;        // 总开关实际反馈值

    uint16_t arr_[8]{};           // Modbus 寄存器批量读取缓冲区
};

#endif // DIDO_H