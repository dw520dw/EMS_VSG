// =====================================================================
// BAMU.h
// 电池管理主控单元（BAMU）头文件
// 负责 BMS 电芯数据采集、空调通信告警、DI 信号读取及逻辑数据写入
// =====================================================================

#ifndef BAMU_H
#define BAMU_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <array>
#include <chrono>
#include <memory>
#include <string>

// ========== 全局常量 ==========

// 缩放系数，用于寄存器原始值换算
const double BamuRatio_Value3 = 0.001;

// BMS 电芯电压采集点数
static constexpr int BMS_CELL_VOLTAGE_COUNT = 260;
// BMS 电芯温度采集点数
static constexpr int BMS_CELL_TEMPERATURE_COUNT = 135;
// BMS 电芯 Modbus 读取块大小（主块）
static constexpr int BMS_CELL_MODBUS_BLOCK = 120;
// BMS 电芯 Modbus 读取块大小（子块1）
static constexpr int BMS_CELL_MODBUS_BLOCK1 = 15;
// BMS 电芯 Modbus 读取块大小（子块2）
static constexpr int BMS_CELL_MODBUS_BLOCK2 = 20;
// 最大 BMS 簇数量
static constexpr int BAMU_MAX_BMS_CLUSTER = 10;
// 每个空调单元对应的 BMS 簇数量
static constexpr int BMS_CLUSTERS_PER_AIR_UNIT = 2;
// 最大空调单元数量（由 BMS 簇数量推算）
static constexpr int BAMU_MAX_AIR_UNIT = (BAMU_MAX_BMS_CLUSTER + 1) / 2;
// 空调通信告警寄存器基地址
static constexpr int AIR_COM_ALARM_ADDR_BASE = 7;
// DI（数字量输入）起始地址
static constexpr int DI_START_ADDR = 0x898;
// DI 信号数量
static constexpr int DI_COUNT = 13;

// ========== 内联工具函数 ==========

// 校验 BMS 簇 ID 是否合法（1 ~ BAMU_MAX_BMS_CLUSTER）
inline bool isValidBmsClusterId(int id) { return id >= 1 && id <= BAMU_MAX_BMS_CLUSTER; }

// 将 BMS 簇数量限制在合法范围内
inline int clampBmsClusterCount(int n)
{
    if (n < 1) return 1;
    if (n > BAMU_MAX_BMS_CLUSTER) return BAMU_MAX_BMS_CLUSTER;
    return n;
}

// 校验空调单元 ID 是否合法（1 ~ BAMU_MAX_AIR_UNIT）
inline bool isValidAirUnitId(int id) { return id >= 1 && id <= BAMU_MAX_AIR_UNIT; }

// 根据 BMS 簇 ID 计算对应的空调单元 ID（每 2 个簇对应 1 个空调单元）
inline int bmsClusterToAirUnitId(int id)
{
    return (id + BMS_CLUSTERS_PER_AIR_UNIT - 1) / BMS_CLUSTERS_PER_AIR_UNIT;
}

// 判断该 BMS 簇是否为空调单元的主簇（每组第一个）
inline bool isAirUnitLeadCluster(int id)
{
    return (id - 1) % BMS_CLUSTERS_PER_AIR_UNIT == 0;
}

// 根据空调单元 ID 计算通信告警寄存器地址
inline int airUnitToComAlarmAddr(int id) { return AIR_COM_ALARM_ADDR_BASE + id - 1; }

// 根据空调单元 ID 计算逻辑通信寄存器地址（每个单元间隔 1000）
inline int airUnitToLogicCommAddr(int id) { return 201 + (id - 1) * 1000; }

// ========== 数据结构 ==========

// BMS 电芯数据（电压 + 温度）
struct BMSCellData {
    // 电芯电压数组，单位由缩放系数决定
    uint16_t Cell_Voltage[BMS_CELL_VOLTAGE_COUNT]{};
    // 电芯温度数组，单位由缩放系数决定
    uint16_t Cell_Temperature[BMS_CELL_TEMPERATURE_COUNT]{};
};

// DI（数字量输入）信号集合
struct DiSignals {
    bool EPO = false;           // 紧急停机信号
    bool TotalSwitch = false;   // 总开关信号
    bool FireLevel1 = false;    // 消防一级报警信号
    bool FireControl = false;   // 消防控制信号
};

// ========== Bamu 类 ==========

class Bamu {
public:
    // 构造函数，加载配置并建立 Modbus TCP 连接
    explicit Bamu(const std::string& configPath = std::string());
    // 析构函数，释放资源
    ~Bamu();
    // 主循环线程，周期性轮询 BMS/空调/堆栈数据
    void runBamuDataThread(MySQLConnectionPool& pool);

private:
    // 堆栈数据后处理回调
    void onAfterStack(ModbusPollEngine& eng, MySQLDatabase& db);
    // BMS 数据后处理回调（电芯电压/温度采集）
    void onAfterBms(ModbusPollEngine& eng, MySQLDatabase& db);
    // 空调数据后处理回调（通信告警状态）
    void onAfterAir(ModbusPollEngine& eng, MySQLDatabase& db);
    // 计算当前告警等级
    int computeAlarmLevel(const ModbusPollEngine& eng) const;
    // 读取 DI 数字量输入信号
    bool readDiSignals(DiSignals& out);
    // 将逻辑数据写入数据库
    void writeLogicData(MySQLDatabase& db, const ModbusPollEngine& stack);
    // 读取指定簇的电芯数据（电压 + 温度）
    void readBmsCellData(int id);
    // 将指定簇的电芯数据写入数据库
    void writeCellData(MySQLDatabase& db, int id);
    // 处理堆栈数据（轮询 + 后处理）
    void processStack(MySQLDatabase& db);
    // 处理指定簇的 BMS 数据
    void processCluster(MySQLDatabase& db, int id);
    // 将数据库列表数据批量写入指定表
    void flushDbList(MySQLDatabase& db, const std::string& table);

    // Modbus TCP 通信总线
    std::unique_ptr<ModbusTCP> bus_;
    // 堆栈数据轮询引擎
    std::unique_ptr<ModbusPollEngine> stackEngine_;
    // BMS 电芯数据轮询引擎
    std::unique_ptr<ModbusPollEngine> bmsEngine_;
    // 空调数据轮询引擎
    std::unique_ptr<ModbusPollEngine> airEngine_;

    int batteryNumber_ = 1;     // 电池编号
    int qtPollCounter_ = 0;     // 轮询计数器（用于控制采集频率）
    DiSignals diSignals_{};     // 当前 DI 信号状态
    databaseList dbList_;       // 数据库写入列表

    // 输入寄存器读取缓冲区
    uint16_t arr_input_registers_[128]{};
    // 输入位（线圈）读取缓冲区
    uint8_t arr_input_bits_[32]{};

    // 各簇电芯数据缓存（索引 0 保留，1~MAX 对应簇 ID）
    std::array<BMSCellData, BAMU_MAX_BMS_CLUSTER + 1> bmsCell_{};
    // 各簇电芯数据上次采集时间戳
    std::array<std::chrono::steady_clock::time_point, BAMU_MAX_BMS_CLUSTER + 1> lastCellRealtime_{};
    // 各簇电芯电压上次写入数据库的值（用于变化检测）
    std::array<std::array<double, BMS_CELL_VOLTAGE_COUNT + 1>, BAMU_MAX_BMS_CLUSTER + 1> lastVolDb_{};
    // 各簇电芯温度上次写入数据库的值（用于变化检测）
    std::array<std::array<double, BMS_CELL_TEMPERATURE_COUNT + 1>, BAMU_MAX_BMS_CLUSTER + 1> lastTempDb_{};
    // 各簇是否已有电压快照（首次写入标记）
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasVolSnap_{};
    // 各簇是否已有温度快照（首次写入标记）
    std::array<bool, BAMU_MAX_BMS_CLUSTER + 1> hasTempSnap_{};
    // 各空调单元在线状态
    std::array<double, BAMU_MAX_AIR_UNIT + 1> airOnline_{};
};

#endif