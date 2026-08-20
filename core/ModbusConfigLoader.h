#ifndef MODBUS_CONFIG_LOADER_H
#define MODBUS_CONFIG_LOADER_H

#include "ModbusDeviceProfile.h"
#include <string>

/**
 * 从 Modbus 配置加载一台逻辑设备。
 *
 * 推荐布局：
 *   config/modbus/devices.json
 *   config/modbus/templates/<template名>.json   — 纯读点表
 *   config/modbus/sink/<template名>.json        — 独立写库映射
 *
 * devices.json:
 *   { "devices": [ { "id":"ess_meter", "template":"adl_meter", ... } ] }
 *
 * templates/adl_meter.json（只含读侧，无写侧字段）:
 *   {
 *     "read": {
 *       "fields": [
 *         { "type":"virtual_comm" },
 *         { "fc":3, "address":"0x61", "type":"i16", "scale":0.1 }
 *       ]
 *     }
 *   }
 *
 * sink/adl_meter.json（独立写库映射，显式 {name,addr}）:
 *   {
 *     "field_maps": { "meter": [ { "name":"Online", "addr":0 }, ... ] },
 *     "tables": [ { "mysql":"ess_meter", "field_map":"meter" } ]
 *   }
 *
 * 兼容旧单文件（根上带 "templates":{...}）。
 * 引擎按 fc+address 自动合并连续读请求；name 可选（仅 hook）。
 */
ModbusDeviceProfile loadModbusDeviceProfile(const std::string& jsonPath,
                                            const std::string& deviceId);

#endif  // MODBUS_CONFIG_LOADER_H
