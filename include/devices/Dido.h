#ifndef DIDO_H
#define DIDO_H

#include <chrono>
#include <cstdint>
#include <string>
#include "MySQLDB_1.h"
#include "influxDB.h"
#include "ModbusRtu.h"

// 定义DI结构体，用于存储数字输入的状态
struct STR_INPUTDI {
    bool ChiFa_Switch{false}; //柴发开关反馈
    bool PV_Switch{false}; //光伏开关反馈
    bool Load_Switch{false}; //负载开关反馈
    bool ChiFa_Singn{false}; //柴发信号
};

// 定义DO结构体，用于存储数字输出的状态
struct STR_OUTPUTDO {
    bool yellowLed{false};  // 黄色LED状态
    bool greenLed{false};   // 绿色LED状态
    bool redLed{false};     // 红色LED状态
    bool ChiFa_Switch{false}; //柴发开关
    bool QacSplit{false};    //QAC分励
};

constexpr int COMM_ERROR_THRESHOLD = 3;      // 通信错误阈值
constexpr int DIDO_THREAD_SLEEP_MS = 500;      // 线程睡眠时间(毫秒)

// LED/DO 控制位定义
constexpr int GREEN_LED_BIT = 1;
constexpr int YELLOW_LED_BIT = 0;
constexpr int RED_LED_BIT = 2;
constexpr int CHIFA_SWITCH_BIT = 3;
constexpr int QAC_SPLIT_BIT = 4;


class Dido
{
public:
    Dido(const char* device_ip,int device_port, int device_address);  
    ~Dido();
    void didoThread(MySQLConnectionPool& pool);

private:
    bool handleCommunicationError(int& errorCount);
    void staterun(int& errorCount);
    void handleExternalData(MySQLConnectionPool& pool);
    void ioDoData(MySQLConnectionPool& pool);
    void iodoHistory();
    ModbusTCP modbusclient;
    databaseList List;
    DatabaseList listhistory;
    influxDB DB;
    STR_INPUTDI DI;
    STR_OUTPUTDO DO;
    STR_OUTPUTDO readDO;
    uint16_t arr_uint16[50] = {0};
    std::string tableNameIODO = "dodi";
    std::string tableNameLogic = "logic";
    int lastRegisterValue = -1;
    int lastbootvalue = -1;
    int commErrCount = 0;
    std::chrono::steady_clock::time_point lastHistoryTime{};
};

#endif  // DIDO_H
