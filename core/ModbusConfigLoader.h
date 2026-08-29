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
 * templates/amc_meter.json（只含真实寄存器，无 virtual / virtual_comm）:
 *   {
 *     "read": {
 *       "fields": [
 *         { "fc":3, "address":"0x61", "type":"i16", "scale":0.1, "name":"APhaseVoltage" }
 *       ]
 *     }
 *   }
 *
 * sink/amc_meter.json（独立写库映射；派生点如 Status/Power 只出现在这里）:
 *   {
 *     "field_maps": { "amc_meter": [ { "name":"APhaseVoltage", "addr":1 }, ... ] },
 *     "tables": [ { "mysql":"ess_meter", "field_map":"amc_meter" } ]
 *   }
 * 通讯在线由 devices.json comm.mysql_online_addr 写入，不进 templates/sink。
 *
 * 兼容旧单文件（根上带 "templates":{...}）。
 * 引擎按 fc+address 自动合并连续读请求；name 可选（仅 hook）。
 */
ModbusDeviceProfile loadModbusDeviceProfile(const std::string& jsonPath,
                                            const std::string& deviceId);

#endif  // MODBUS_CONFIG_LOADER_H
