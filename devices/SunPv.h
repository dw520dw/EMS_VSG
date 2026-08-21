
#ifndef SUNPV_H
#define SUNPV_H

// ========== 文件说明 ==========
// SunPv 光伏逆变器 Modbus 通信驱动头文件
// 功能：定义 SunPv 类接口，通过 Modbus RTU/TCP 与光伏逆变器通信，
//       采集运行状态、告警信息，并支持有功功率限制指令下发。支持最多 8 台逆变器轮询。

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <array>
#include <memory>
#include <string>

// ========== SunPv 类定义 ==========
class SunPv {
public:
    // 最大支持的逆变器数量
    static constexpr std::size_t kMaxInverters = 8;

    // 通信告警寄存器地址基址（每台逆变器从基址开始递增，偏移量为逆变器编号-1）
    static constexpr int kComAlarmAddrBase = 12;

    // 总功率限制数据寄存器地址（从数据库读取总功率限制值）
    static constexpr int kDataTotalPowerLimitAddr = 110;

    // 逆变器额定功率（单位：kW），用于功率限制指令的工程单位换算
    static constexpr uint16_t kRatedPowerKw = 125;

    // 构造函数：加载 Modbus 设备配置文件，初始化 RTU 通信和轮询引擎
    // 参数 configPath: 配置文件路径，为空时使用 Config 默认路径
    explicit SunPv(const std::string& configPath = std::string());

    // 析构函数：断开 Modbus 连接，释放资源
    ~SunPv();

    // 主循环线程：持续轮询所有逆变器，每秒（或配置的 poll_ms）执行一轮
    // 参数 pool: MySQL 连接池引用
    void runSunPvThread(MySQLConnectionPool& pool);

private:
    // 解码后回调：Modbus 数据解码完成后自动调用，
    // 将原始寄存器值转换为语义化字段（状态码、告警位等）并写入数据库
    // 参数 eng: Modbus 轮询引擎引用
    // 参数 db:  MySQL 数据库连接
    void onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db);

    // 轮询单台逆变器：设置从站地址，探测设备是否在线，
    // 在线则执行一次完整轮询，离线则标记离线状态
    // 参数 db:     MySQL 数据库连接
    // 参数 amount: 逆变器编号（从 1 开始）
    void pollOneInverter(MySQLDatabase& db, int amount);

    // 有功功率限制下发：计算单台逆变器的功率限制目标值，
    // 通过 Modbus 寄存器 5007 写入，使用死区机制避免频繁写入
    // 参数 eng: Modbus 轮询引擎引用
    void writePowerLimit(ModbusPollEngine& eng);

    // 帧间隔延时：在连续轮询多台逆变器时，按配置的 inter_frame_ms 延时，
    // 避免串口总线冲突
    void sleepFrameGap() const;

    // ========== 成员变量 ==========

    // Modbus RTU 通信对象（独占智能指针）
    std::unique_ptr<ModbusRTU> bus_;

    // Modbus 轮询引擎（独占智能指针，管理寄存器读取、解码、缓存等）
    std::unique_ptr<ModbusPollEngine> engine_;

    // 每台逆变器的通信错误计数器（通信失败时递增，超过 3 次标记为离线）
    std::array<int, kMaxInverters> commErrCount_{};

    // 总功率限制值（从数据库 data_total 表读取，单位：W）
    uint16_t himActivePowerLimit_ = 0;

    // 逆变器模块数量（从数据库 qt 表读取，实际在线的逆变器台数）
    uint16_t numberOfModules_ = 0;

    // 探测器缓冲区（用于 probe 指令的返回值暂存）
    uint16_t probeBuf_ = 0;
};

#endif