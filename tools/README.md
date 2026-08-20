# EMS_VSG 配置工具

## 配置结构（读/写分离后）

```
config/modbus/
  devices.json                # 设备连接参数 + mysql_table（内容不含写库映射）
  templates/<name>.json       # 纯读点表 read.fields[]（fc/address/type/scale…，无写侧字段）
  sink/<name>.json            # 独立写库映射（field_maps 显式 {name,addr} + tables 路由）
```

写库映射与寄存器点表彻底分离：模板只描述"设备有什么寄存器"，sink 只描述"写到哪个表哪个地址"。
4 个 amc_meter（load/dg/pv/ess）共享同一份 `sink/amc_meter.json` 写序。

## 依赖

```bash
python -m pip install openpyxl
```

## Excel ↔ JSON（json2xlsx / xlsx2json）

```bash
# 由 JSON 生成 Excel 模板
python tools/json2xlsx.py                       # → config/modbus/EMS_VSG配置模板.xlsx

# 填好的 Excel 转回 JSON（先预览，再真生成，默认覆盖 config/modbus）
python tools/xlsx2json.py config/modbus/EMS_VSG配置模板.xlsx --dry-run
python tools/xlsx2json.py config/modbus/EMS_VSG配置模板.xlsx
```

Excel 含 4 类 Sheet：
- **设备配置**：一行一台设备（devices.json）
- **每模板一个 Sheet**：第 1 行表头起 = 采集点（纯读）
- **写库映射**：`template | field_map | name | addr`，一行一个写点
- **写库表路由**：`template | field_map | mysql | mysql_prefix`，一行一条路由

写序列表与表路由分离：amc_meter 的写序只列一次，4 张表通过路由共享。

## 迁移（migrate_sink.py）

把旧版耦合配置（模板含 sink/db_addr/write_mysql）一次性迁移为新结构，**保证 MySQL addr 零漂移**：

```bash
# dry-run + 与历史上传映射交叉核对
python tools/migrate_sink.py --dry-run --verify-history ../EMS_VSG_history_uploader/history_config

# 生成到 staging 目录
python tools/migrate_sink.py --out <staging> --verify-history <history_config> --write-manifest <staging>/sink_manifest.json

# 核对无误后覆盖 config/modbus
```

## 校验（validate_sink.py）

```bash
python tools/validate_sink.py                       # 校验 devices/templates/sink 自洽
python tools/validate_sink.py --manifest sink_manifest.json   # 额外与迁移审计逐点比对
```

校验项：每设备能路由到 field_map、写点 name 在模板中存在、addr≥0 且不重复、模板都有 sink。

## 字段说明

- `type` 枚举：`u16 i16 u32_hi_lo i32_hi_lo u32_lo_hi i32_lo_hi bit reg_bit virtual_comm virtual`
- `fc`：`1/2/3/4`（coil / discrete / holding / input）
- `address`：十进制或 `"0x.."` 十六进制
- 写库映射 `{name, addr}` 为**显式地址**：`addr` 直接写死，与模板数组顺序解耦
- 多簇设备（sun_pv/agc/bms/air）用 `mysql_prefix` 路由，同一写序复用于所有簇
- 内部点（如 `_st*`/`_pad*`，旧 `write_mysql:false`）留在模板供 hook 读取，**不出现在写库映射**

## 注意

- 目标板配置按 mtime 缓存、**无热加载**，改配置需重启 collect 进程。
- 旧版 Excel（含 sink 头两行）与新版工具不兼容，需重新生成。
- C++ 加载器对缺失/无法路由的 sink 文件会抛异常（启动即报错），上线前先本地 validate。
