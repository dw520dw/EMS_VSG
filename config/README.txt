Modbus 配置（读寄存器与写库分离）

  devices.json              — 各逻辑设备连接参数、引用的 template 名
  templates/<name>.json     — 纯读点表（read.fields，无写侧字段）
  sink/<name>.json          — 独立写库映射（field_maps 显式 {name,addr} + tables 路由）

设备与模板对应：
  load_meter / dg_meter / pv_meter / ess_meter  → amc_meter
  sun_pv / agc / pcs_smarten / dido             → 同名模板
  bamu_stack / bamu_bms / bamu_air              → 同名模板

改 IP/串口/slave → 只改 devices.json
改寄存器点表     → 只改对应 templates/*.json
改 MySQL addr    → 只改对应 sink/*.json
通讯在线         → devices.json comm.mysql_online_addr（空调 Online 是真实 DI 位，仍在模板）
电表乘 PT/CT     → templates/amc_meter.json 根上 pt_ct 名单

程序入口：Config::MODBUS_DEVICES_CONFIG → devices.json
