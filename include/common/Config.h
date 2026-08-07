#ifndef CONFIG_H
#define CONFIG_H

namespace Config {
    // 串口配置
    const char* const PORT0 = "/usr/dev/serial/com1";
    const char* const PORT1 = "/usr/dev/serial/com2";
    const char* const PORT2 = "/usr/dev/serial/com3";
    const char* const PORT3 = "/usr/dev/serial/com4";
    const char* const PORT4 = "/usr/dev/serial/com5";
    const char* const PORT5 = "/usr/dev/serial/com6";
    const int BAUD_RATE_NORMAL = 9600;
    const int BAUD_RATE_HIGH   = 115200;

    // 数据库配置
    const char* const DB_HOST     = "localhost";
    const char* const DB_USER     = "zt";
    const char* const DB_PASSWORD = "zt";
    const char* const DB_NAME     = "ems";
    const int DB_POOL_SIZE        = 15;

    // 线程延时配置（单位：毫秒）
    const int BAMU_DATA_INTERVAL   = 600;   // BAMU/BMS 主循环采集周期
    const int BMS_CELL_REALTIME_INTERVAL = 10000;  // 单体电压/温度实时库写入周期
    const int PCS_JUDGMENT_DELAY  = 600;   // PCS 数据采集周期
    const int SUN_PV_DATA_INTERVAL = 600;   // 光伏逆变器数据采集周期
    const int AGC_DATA_INTERVAL    = 600;   // 柴发 AGC 数据采集周期
    const int METER_DATA_INTERVAL  = 600;   // 电表数据采集周期
    const int METER_SLAVE_SWITCH_DELAY_MS = 10;  // RTU 同总线切换从站间隔（电表/柴发等）
    const int METER_REGISTER_READ_DELAY_MS = 5;  // 485 每组寄存器读完后间隔
    const int THREAD_DELAY        = 50;

    // BAMU/BMS/PCS/DIDO 设备配置
    const char* const BAMU_BMS_IP  = "192.168.1.210";
    const char* const PCS_IP       = "192.168.1.201";
    const char* const DIDO_IP      = "127.0.0.1";
    const int BAMU_BMS_PORT        = 502;
    const int PCS_PORT             = 502;
    const int DIDO_PORT            = 5020;
    const int ADDRESS_1          = 1;
    const int ADDRESS_2          = 2;
    const int ADDRESS_3          = 3;
    const int ADDRESS_4          = 4;
    const int ADDRESS_5          = 5;
    const int ADDRESS_6          = 6;
    const int ADDRESS_7          = 7;
    const int ADDRESS_8          = 8;
    const int ADDRESS_9          = 9;
    const int ADDRESS_10         = 10;
}

#endif // CONFIG_H
