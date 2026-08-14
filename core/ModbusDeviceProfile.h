#ifndef MODBUS_DEVICE_PROFILE_H
#define MODBUS_DEVICE_PROFILE_H

#include "IModbusBus.h"
#include <string>
#include <vector>

/**
 * 点类型：Telegraf 式绝对地址解码。
 * 与 JSON read.fields[].type 对应。
 */
enum class ModbusPointType {
    U16,         // 无符号 16 位寄存器
    I16,         // 有符号 16 位寄存器
    U32_HI_LO,   // 32 位无符号：address=高字，address+1=低字
    I32_HI_LO,   // 32 位有符号，同上字序
    U32_LO_HI,   // 32 位无符号：address=低字，address+1=高字（扩展用）
    I32_LO_HI,   // 32 位有符号，同上字序
    Bit,         // 离散量/线圈：address 为位地址
    RegBit,      // 寄存器 address 内某一 bit（bit 为 0..15）
    VirtualComm, // 引擎注入的通讯在线标志（0/1），不读寄存器
    Virtual      // 由 post-decode hook 填充（功率累计、告警等级等）
};

/**
 * 读层测点（Telegraf 风格）：fc + address + type + scale/bias。
 * 公式：value = raw * scale * (pt ^ pt_exp) * (ct ^ ct_exp) + bias
 * 写库由 ModbusSinkDef 决定（顺序或显式 addr）。
 */
struct ModbusPointDef {
    std::string name;          // 可选；供 hook getValue/setValue；空则加载器生成 _fN
    ModbusFc fc = ModbusFc::HoldingRegisters;
    int address = 0;           // 绝对地址（第 1 簇基址）；运行时可变（多簇）
    int bit = 0;               // RegBit 时字内位号 0..15
    int cluster_stride = 0;    // 多簇：eff = address_base + (clusterIndex-1)*stride
    ModbusPointType type = ModbusPointType::U16;
    double scale = 1.0;
    double bias = 0.0;
    int pt_exp = 0;
    int ct_exp = 0;
    int db_addr = -1;          // 仅 sink=explicit 时使用
    bool write_mysql = true;
};

/** 写库层：与读层解耦 */
enum class ModbusSinkMode {
    Sequential,  // 按 fields 中 write_mysql 顺序：addr = base_addr + 0,1,2...
    Explicit     // 每点自带 db_addr（稀疏表，如 dido）
};

struct ModbusSinkDef {
    ModbusSinkMode mode = ModbusSinkMode::Explicit;
    int base_addr = 0;
};

/** 通讯探测与 Online / com_alarm 写库策略 */
struct ModbusCommAlarmDef {
    bool enabled = true;
    int probe_addr = 0;
    int probe_count = 1;
    int fail_threshold = 3;
    int mysql_online_addr = 0;
    int com_alarm_addr = -1;
    std::string com_alarm_table = "com_alarm";
};

struct ModbusEnableDef {
    bool use_qt = false;
    int qt_addr = -1;
    int enabled_value = 1;
};

struct ModbusPtCtSyncDef {
    bool enabled = false;
    int pt_qt_addr = -1;
    int ct_qt_addr = -1;
    int pt_reg = -1;
    int ct_reg = -1;
    int read_reg = -1;
    int min_value = 1;
    int max_value = 30000;
};

/**
 * 一台逻辑设备的完整配置（由 JSON devices[] + templates 解析得到）。
 */
struct ModbusDeviceProfile {
    std::string id;
    std::string transport;
    std::string rtu_device;
    int rtu_baud = 9600;
    std::string tcp_ip;
    int tcp_port = 502;
    int slave = 1;
    std::string mysql_table;
    int table_suffix = 0;
    /** 子单元数量所在 qt 表地址（AGC 柴发并机台数 / SunPv 模块数 / Bamu 电池簇数；0=未配置，见 devices.json count_qt_addr） */
    int count_qt_addr = 0;
    int poll_ms = 50;
    int inter_frame_ms = 0;
    /** Modbus 应答超时（毫秒）；0=用传输层默认（TCP 5000 / RTU 1000） */
    int timeout_ms = 0;
    double default_pt = 1.0;
    double default_ct = 1.0;
    ModbusCommAlarmDef comm;
    ModbusEnableDef enable;
    ModbusPtCtSyncDef pt_ct_sync;
    ModbusSinkDef sink;
    std::vector<ModbusPointDef> points;  // 读层 fields

    std::string resolvedMysqlTable() const
    {
        if (table_suffix > 0)
        {
            return mysql_table + std::to_string(table_suffix);
        }
        return mysql_table;
    }
};

#endif  // MODBUS_DEVICE_PROFILE_H
