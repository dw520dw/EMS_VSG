Modbus 配置（设备清单 + 点表模板分离）

  devices.json              — 各逻辑设备连接参数、引用的 template 名
  templates/<name>.json     — 点表（read.fields + sink）

设备与模板对应：
  ess_meter / grid_meter  → adl_meter（共用）
  pcs_ylk                 → pcs_ylk
  bamu_stack / bamu_bms / bamu_air_* → 同名模板
  dido_1 / dido_2         → dido_slave1 / dido_slave2

改 IP/串口/slave → 只改 devices.json
改寄存器点表     → 只改对应 templates/*.json

程序入口：Config::MODBUS_DEVICES_CONFIG → devices.json
