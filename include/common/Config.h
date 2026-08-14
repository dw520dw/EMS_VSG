#ifndef CONFIG_H
#define CONFIG_H

namespace Config {
    // 数据库
    const char* const DB_HOST     = "localhost";
    const char* const DB_USER     = "zt";
    const char* const DB_PASSWORD = "zt";
    const char* const DB_NAME     = "ems";
    const int DB_POOL_SIZE        = 15;

    // BAMU / 单体节奏（业务层，点表在 JSON）
    const int BAMU_DATA_INTERVAL         = 600;
    const int BMS_CELL_REALTIME_INTERVAL = 10000;
    const int BAMU_QT_CONFIG_POLL_INTERVAL = 10;

    // Modbus：连接见 devices.json，点表见 templates/
    const char* const MODBUS_DEVICES_CONFIG = "config/modbus/devices.json";

    const char* const LOAD_METER_DEVICE_ID = "load_meter";
    const char* const DG_METER_DEVICE_ID   = "dg_meter";
    const char* const PV_METER_DEVICE_ID   = "pv_meter";
    const char* const ESS_METER_DEVICE_ID  = "ess_meter";
    const char* const PCS_SMARTEN_DEVICE_ID = "pcs_smarten";
    const char* const BAMU_STACK_DEVICE_ID  = "bamu_stack";
    const char* const BAMU_BMS_DEVICE_ID    = "bamu_bms";
    const char* const BAMU_AIR_DEVICE_ID    = "bamu_air";
    const char* const DIDO_DEVICE_ID        = "dido";
    const char* const SUN_PV_DEVICE_ID      = "sun_pv";
    const char* const AGC_DEVICE_ID         = "agc";
}

#endif // CONFIG_H
