# EMS_VSG 采集程序（配置驱动）

## 架构

- 采集只写 MySQL 实时库；连接/周期/点表见 `config/modbus/`
- 历史上传独立工程：`../EMS_VSG_history_uploader`

## 配置

- `config/modbus/devices.json`：串口/IP/从站/周期/表名
- `config/modbus/templates/*.json`：寄存器点表与落库顺序

改现场参数后重启进程即可，无需重编译。

## 构建

```bash
make arm
```

运行（可选指定 devices.json 路径）：

```bash
./collect
./collect /userdata/iEMS-MG1000/config/modbus/devices.json
```

部署时请把整个 `config/modbus/` 目录放到板端，工作目录能访问该路径（或传绝对路径）。
