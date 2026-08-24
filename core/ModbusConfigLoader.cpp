/**
 * Modbus 统一配置加载器。
 * Telegraf 式 read.fields（fc/address/type/scale）纯读点表 + 独立 sink 写库映射。
 *
 * 布局（推荐）：
 *   config/modbus/devices.json          — 设备连接与 template 名
 *   config/modbus/templates/<name>.json — 纯读点表（read.fields，无写侧字段）
 *   config/modbus/sink/<name>.json      — 独立写库映射（field_maps {name,addr} + tables 路由）
 *
 * 兼容旧单文件：根上仍可带 "templates": { ... }。
 * 各 JSON 按 path+mtime 缓存，多设备重复加载不反复读盘。
 */

#include "ModbusConfigLoader.h"
#include "json.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>

using json = nlohmann::json;
namespace fs = std::filesystem;

namespace {

/** 解析 fc：整数 1/2/3/4 或字符串 "coils"/"discrete"/"holding"/"input" → ModbusFc 枚举；非法抛异常。 */
ModbusFc parseFc(const json& j)
{
    if (j.is_number_integer())
    {
        const int v = j.get<int>();
        switch (v)
        {
        case 1:
            return ModbusFc::Coils;
        case 2:
            return ModbusFc::DiscreteInputs;
        case 3:
            return ModbusFc::HoldingRegisters;
        case 4:
            return ModbusFc::InputRegisters;
        default:
            break;
        }
    }
    else if (j.is_string())
    {
        const std::string s = j.get<std::string>();
        if (s == "coils" || s == "coil")
            return ModbusFc::Coils;
        if (s == "discrete" || s == "discrete_inputs")
            return ModbusFc::DiscreteInputs;
        if (s == "holding" || s == "holding_registers")
            return ModbusFc::HoldingRegisters;
        if (s == "input" || s == "input_registers")
            return ModbusFc::InputRegisters;
    }
    throw std::runtime_error("invalid Modbus fc in config");
}

/** 解析点类型字符串 → ModbusPointType 枚举（u16/i16/u32_hi_lo/…/virtual）；非法抛异常。 */
ModbusPointType parsePointType(const std::string& s)
{
    if (s == "u16")
        return ModbusPointType::U16;
    if (s == "i16")
        return ModbusPointType::I16;
    if (s == "u32" || s == "u32_hi_lo")
        return ModbusPointType::U32_HI_LO;
    if (s == "i32" || s == "i32_hi_lo")
        return ModbusPointType::I32_HI_LO;
    if (s == "u32_lo_hi")
        return ModbusPointType::U32_LO_HI;
    if (s == "i32_lo_hi")
        return ModbusPointType::I32_LO_HI;
    if (s == "bit")
        return ModbusPointType::Bit;
    if (s == "reg_bit")
        return ModbusPointType::RegBit;
    if (s == "virtual_comm" || s == "comm")
        return ModbusPointType::VirtualComm;
    if (s == "virtual")
        return ModbusPointType::Virtual;
    throw std::runtime_error("invalid point type: " + s);
}

/** 十进制或 "0x.." 字符串地址 */
int parseAddr(const json& j)
{
    if (j.is_string())
    {
        return std::stoi(j.get<std::string>(), nullptr, 0);
    }
    return j.get<int>();
}

/**
 * 解析读层 fields 数组（Telegraf 式绝对地址）。
 *
 * 每项典型形态：
 *   { "fc":3, "address":"0x61", "type":"i16", "scale":0.1, "pt_exp":1, "name":"可选" }
 *
 * 字段说明：
 * - name           可选；给 C++ hook 的 getValue/setValue 用；缺省则生成 _f0/_f1...
 * - type           解码类型；默认 u16。virtual / virtual_comm 不读寄存器，无需 fc/address
 * - fc + address   真实测点必填；address 支持十进制或 "0x.." 字符串
 * - cluster_stride 多簇地址步进（BMS 等）；运行时 address += (簇号-1)*stride
 * - bit            仅 reg_bit：字内位号 0..15
 * - scale / bias   工程值标定：raw * scale * PT^pt_exp * CT^ct_exp + bias
 * - pt_exp / ct_exp 是否乘以电表 PT/CT（0=不乘）
 *
 * 写库映射不在模板中：由 config/modbus/sink/<template>.json 的 field_maps 显式 {name,addr} 决定。
 */
void parseFields(const json& arr, std::vector<ModbusPointDef>& out)
{
    out.clear();
    for (const auto& pt : arr)
    {
        ModbusPointDef point;

        if (pt.contains("name") && pt["name"].is_string())
        {
            point.name = pt["name"].get<std::string>();
        }
        if (point.name.empty())
        {
            point.name = "_f" + std::to_string(out.size());
        }

        point.type = parsePointType(pt.value("type", std::string("u16")));

        if (point.type != ModbusPointType::VirtualComm && point.type != ModbusPointType::Virtual)
        {
            if (!pt.contains("fc") || !pt.contains("address"))
            {
                throw std::runtime_error("field " + point.name +
                                         ": need fc+address (Telegraf style)");
            }
            point.fc = parseFc(pt["fc"]);
            point.address = parseAddr(pt["address"]);
            point.cluster_stride = pt.value("cluster_stride", 0);
            point.bit = pt.value("bit", 0);
        }

        point.scale = pt.value("scale", 1.0);
        point.bias = pt.value("bias", 0.0);
        point.pt_exp = pt.value("pt_exp", 0);
        point.ct_exp = pt.value("ct_exp", 0);

        out.push_back(std::move(point));
    }
}

/** 读取 read.fields；兼容设备根上直接写 fields */
void parseReadSection(const json& node, std::vector<ModbusPointDef>& points)
{
    const json& src = node.contains("read") ? node["read"] : node;
    if (!src.contains("fields"))
    {
        throw std::runtime_error("missing read.fields");
    }
    parseFields(src["fields"], points);
}

/** 校验 transport 只允许 rtu|tcp；tcp_ip/rtu_device 等连接参数由业务侧建总线时校验。 */
void validateTransport(const ModbusDeviceProfile& p)
{
    if (p.transport != "tcp" && p.transport != "rtu")
    {
        throw std::runtime_error("device " + p.id + ": unknown transport '" + p.transport +
                                 "' (expect rtu|tcp)");
    }
    // tcp_ip / rtu_device：建总线的主设备由业务侧校验（如 Bamu 查 bamu_stack）；
    // 从设备可省略（与主设备共用同一 IModbusBus）。
}

struct JsonFileCache {
    std::mutex mu;
    std::unordered_map<std::string, std::pair<fs::file_time_type, json>> files;
};

/** 返回按 path+mtime 的 JSON 全局缓存单例（带锁）。 */
JsonFileCache& jsonCache()
{
    static JsonFileCache cache;
    return cache;
}

/** 按路径+mtime 缓存 JSON；返回副本，避免跨线程持有缓存引用 */
json loadJsonCached(const std::string& jsonPath)
{
    auto& cache = jsonCache();
    std::lock_guard<std::mutex> lock(cache.mu);

    const fs::path p(jsonPath);
    if (!fs::exists(p))
    {
        throw std::runtime_error("cannot open Modbus config: " + jsonPath);
    }
    const auto mt = fs::last_write_time(p);
    auto it = cache.files.find(jsonPath);
    if (it != cache.files.end() && it->second.first == mt)
    {
        return it->second.second;
    }

    std::ifstream in(jsonPath);
    if (!in)
    {
        throw std::runtime_error("cannot open Modbus config: " + jsonPath);
    }
    json root;
    in >> root;
    cache.files[jsonPath] = {mt, root};
    return root;
}

/**
 * 解析 template：优先 devices.json 同目录下 templates/<name>.json；
 * 否则用旧单文件内嵌的 templates 对象。
 */
json resolveTemplate(const fs::path& devicesPath, const json& devicesRoot,
                     const std::string& tplName)
{
    const fs::path fileTpl =
        devicesPath.parent_path() / "templates" / (tplName + ".json");
    if (fs::exists(fileTpl))
    {
        return loadJsonCached(fileTpl.string());
    }

    if (devicesRoot.contains("templates") && devicesRoot["templates"].contains(tplName))
    {
        return devicesRoot["templates"].at(tplName);
    }

    throw std::runtime_error("unknown template: " + tplName +
                             " (expected " + fileTpl.string() +
                             " or inline templates." + tplName + ")");
}

/**
 * 加载独立写库映射 config/modbus/sink/<template>.json：
 * 按设备 mysql_table 路由到 field_map，校验后物化为 ModbusWritePoint 列表。
 */
void loadSinkWriteMap(const fs::path& devicesPath, const std::string& tplName,
                      const ModbusDeviceProfile& p, std::vector<ModbusWritePoint>& out)
{
    out.clear();
    if (tplName.empty())
    {
        throw std::runtime_error("device " + p.id + ": 无 template，无法解析写库映射 sink");
    }

    const fs::path sinkPath = devicesPath.parent_path() / "sink" / (tplName + ".json");
    if (!fs::exists(sinkPath))
    {
        throw std::runtime_error("device " + p.id + ": 缺少写库映射 " + sinkPath.string());
    }

    const json sink = loadJsonCached(sinkPath.string());
    const json& maps = sink.value("field_maps", json::object());
    const json& tables = sink.value("tables", json::array());

    // 路由：① mysql 精确命中 → ② mysql_prefix 命中 → ③ 唯一 field_map 兜底(警告)
    std::string mapName;
    if (tables.is_array())
    {
        for (const auto& t : tables)
        {
            if (t.value("mysql", std::string()) == p.mysql_table ||
                t.value("mysql_prefix", std::string()) == p.mysql_table)
            {
                mapName = t.value("field_map", std::string());
                break;
            }
        }
    }
    if (mapName.empty() && maps.is_object() && maps.size() == 1)
    {
        mapName = maps.begin().key();
        std::cerr << "device " << p.id << ": sink tables 未命中 mysql_table=" << p.mysql_table
                  << "，用唯一 field_map 兜底: " << mapName << std::endl;
    }
    if (mapName.empty())
    {
        throw std::runtime_error("device " + p.id + ": 写库映射 " + sinkPath.string() +
                                 " 无法路由到 field_map（tables 未匹配 mysql_table=" +
                                 p.mysql_table + "）");
    }
    if (!maps.contains(mapName) || !maps[mapName].is_array())
    {
        throw std::runtime_error("device " + p.id + ": 写库映射缺少 field_maps[" + mapName + "]");
    }

    std::unordered_map<std::string, bool> known;
    for (const auto& pt : p.points)
    {
        known[pt.name] = true;
    }

    for (const auto& item : maps[mapName])
    {
        ModbusWritePoint wp;
        wp.name = item.value("name", std::string());
        if (wp.name.empty())
        {
            throw std::runtime_error("device " + p.id + ": 写库映射项缺 name");
        }
        if (!known.count(wp.name))
        {
            throw std::runtime_error("device " + p.id + ": 写点 " + wp.name +
                                     " 不在模板 read.fields 中");
        }
        wp.addr = item.value("addr", -1);
        if (wp.addr < 0)
        {
            throw std::runtime_error("device " + p.id + ": 写点 " + wp.name + " addr 非法");
        }
        out.push_back(std::move(wp));
    }
}

/**
 * 解析 devices.json 中单个 device 对象 → ModbusDeviceProfile：
 * 连接参数 + comm/enable/pt_ct_sync + 模板读点（points）+ 设备级 read/fields 覆盖
 * + 独立 sink 写库映射（loadSinkWriteMap → write_points）。
 */
ModbusDeviceProfile parseDeviceObject(const json& dev, const fs::path& devicesPath,
                                      const json& devicesRoot)
{
    ModbusDeviceProfile p;
    p.id = dev.value("id", std::string());
    p.transport = dev.value("transport", std::string("rtu"));
    p.rtu_device = dev.value("rtu_device", std::string());
    p.rtu_baud = dev.value("rtu_baud", 9600);
    p.tcp_ip = dev.value("tcp_ip", std::string());
    p.tcp_port = dev.value("tcp_port", 502);
    p.slave = dev.value("slave", 1);
    p.mysql_table = dev.value("mysql_table", std::string());
    p.table_suffix = dev.value("table_suffix", 0);
    p.poll_ms = dev.value("poll_ms", 50);
    p.inter_frame_ms = dev.value("inter_frame_ms", 0);
    p.timeout_ms = dev.value("timeout_ms", 0);
    p.default_pt = dev.value("default_pt", 1.0);
    p.default_ct = dev.value("default_ct", 1.0);
    p.count_qt_addr = dev.value("count_qt_addr", 0);

    if (dev.contains("comm"))
    {
        const auto& c = dev["comm"];
        p.comm.enabled = c.value("enabled", true);
        if (c.contains("probe_addr"))
            p.comm.probe_addr = parseAddr(c["probe_addr"]);
        p.comm.probe_count = c.value("probe_count", 1);
        p.comm.fail_threshold = c.value("fail_threshold", 3);
        p.comm.mysql_online_addr = c.value("mysql_online_addr", 0);
        p.comm.com_alarm_addr = c.value("com_alarm_addr", -1);
        p.comm.com_alarm_table = c.value("com_alarm_table", std::string("com_alarm"));
    }

    if (dev.contains("enable"))
    {
        const auto& e = dev["enable"];
        p.enable.use_qt = e.value("use_qt", false);
        p.enable.qt_addr = e.value("qt_addr", -1);
        p.enable.enabled_value = e.value("enabled_value", 1);
    }

    if (dev.contains("pt_ct_sync"))
    {
        const auto& s = dev["pt_ct_sync"];
        p.pt_ct_sync.enabled = s.value("enabled", false);
        p.pt_ct_sync.pt_qt_addr = s.value("pt_qt_addr", -1);
        p.pt_ct_sync.ct_qt_addr = s.value("ct_qt_addr", -1);
        if (s.contains("pt_reg"))
            p.pt_ct_sync.pt_reg = parseAddr(s["pt_reg"]);
        if (s.contains("ct_reg"))
            p.pt_ct_sync.ct_reg = parseAddr(s["ct_reg"]);
        if (s.contains("read_reg"))
            p.pt_ct_sync.read_reg = parseAddr(s["read_reg"]);
        p.pt_ct_sync.min_value = s.value("min_value", 1);
        p.pt_ct_sync.max_value = s.value("max_value", 30000);
    }

    std::string tplName;
    if (dev.contains("template"))
    {
        tplName = dev.at("template").get<std::string>();
        const json tpl = resolveTemplate(devicesPath, devicesRoot, tplName);
        parseReadSection(tpl, p.points);
    }

    // 设备级 read/fields 可覆盖 template（写库映射仍按 template 解析）
    if (dev.contains("read") || dev.contains("fields"))
    {
        parseReadSection(dev, p.points);
    }

    if (p.points.empty())
    {
        throw std::runtime_error("device " + p.id + ": read.fields empty");
    }
    if (p.id.empty() || p.mysql_table.empty())
    {
        throw std::runtime_error("device missing id/mysql_table");
    }

    validateTransport(p);

    loadSinkWriteMap(devicesPath, tplName, p, p.write_points);
    return p;
}

}  // namespace

/** 对外入口：读 devices.json，按 deviceId 定位并解析成 ModbusDeviceProfile（含写库映射）。 */
ModbusDeviceProfile loadModbusDeviceProfile(const std::string& jsonPath,
                                            const std::string& deviceId)
{
    const json root = loadJsonCached(jsonPath);
    const fs::path devicesPath(jsonPath);

    if (!root.contains("devices") || !root["devices"].is_array())
    {
        throw std::runtime_error(jsonPath + ": missing devices[]");
    }

    for (const auto& dev : root["devices"])
    {
        if (dev.value("id", std::string()) == deviceId)
        {
            return parseDeviceObject(dev, devicesPath, root);
        }
    }
    throw std::runtime_error(jsonPath + ": device id not found: " + deviceId);
}
