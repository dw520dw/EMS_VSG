#include "Dido.h"
#include "logger.h"

Dido::Dido(const char* device_ip, int device_port, int device_address)
    : modbusclient(device_ip, device_port, device_address)
{
}

Dido::~Dido()
{
    modbusclient.disconnect();
}

bool Dido::handleCommunicationError(int& errorCount)
{
    try
    {
        if (!modbusclient.commTest(100, 1, arr_uint16, tableNameIODO))
        {
            errorCount++;
            if (errorCount > COMM_ERROR_THRESHOLD)
                std::cerr << "DIDO 通信错误超过阈值" << std::endl;
            return false;
        }
        errorCount = 0;
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "通信错误处理异常: " << e.what() << std::endl;
        return false;
    }
}

// 第一路DIDO，8路DI,6路DO
void Dido::staterun(int& errorCount)
{
    try
    {
        if (!handleCommunicationError(errorCount))
        {
            return;
        }
        if (modbusclient.readRegisters(100, 4, arr_uint16) != -1)
        {
            // 寄存器 100 为整字 DI 状态，0/非 0 取反后写入逻辑量
            DI.ChiFa_Switch = arr_uint16[0];
            DI.PV_Switch = arr_uint16[1];
            DI.Load_Switch = arr_uint16[2];
            DI.ChiFa_Singn = arr_uint16[3];
        }

        // DO：先读 200~204 设备实际值，再与 out_data 目标比较，有变化才单点写入
        if (modbusclient.readRegisters(200, 5, arr_uint16) != -1) {
            readDO.yellowLed = arr_uint16[0] != 0;
            readDO.greenLed = arr_uint16[1] != 0;
            readDO.redLed = arr_uint16[2] != 0;
            readDO.ChiFa_Switch = arr_uint16[3] != 0;
            readDO.QacSplit = arr_uint16[4] != 0;

            const auto writeDoIfChanged = [this](int regAddr, bool desired, bool& actual,
                                                 const char* pointName) {
                if (desired == actual) {
                    return;
                }
                const uint16_t val = desired ? 1 : 0;
                if (modbusclient.writeRegister(regAddr, val, tableNameIODO) == -1) {
                    return;
                }
                actual = desired;
                LOG_ACTION(std::to_string(regAddr) + (val ? "使能" : "关闭") + pointName);
            };
            writeDoIfChanged(200, DO.yellowLed, readDO.yellowLed, "黄灯");
            writeDoIfChanged(201, DO.greenLed, readDO.greenLed, "绿灯");
            writeDoIfChanged(202, DO.redLed, readDO.redLed, "红灯");
            writeDoIfChanged(203, DO.ChiFa_Switch, readDO.ChiFa_Switch, "柴发开关");
            writeDoIfChanged(204, DO.QacSplit, readDO.QacSplit, "总开关");
        }

    }
    catch (const std::exception& e)
    {
        std::cerr << "DO IO 异常: " << e.what() << std::endl;
    }
}



void Dido::handleExternalData(MySQLConnectionPool& pool)

{

    MySQLDatabase db(pool);
    DO.greenLed = db.select(101, "data_total");
    DO.yellowLed = db.select(102, "data_total");
    DO.redLed = db.select(103, "data_total");
    DO.ChiFa_Switch = db.select(107, "data_total");
    DO.QacSplit = db.select(116, "data_total");
}

void Dido::ioDoData(MySQLConnectionPool& pool)
{
    MySQLDatabase db(pool);
    List.clearData();
    List.addData(1, DI.ChiFa_Switch);
    List.addData(2, DI.PV_Switch);
    List.addData(3, DI.Load_Switch);
    List.addData(8, DI.ChiFa_Singn);
    List.addData(9, DO.yellowLed);
    List.addData(10, DO.greenLed);
    List.addData(11, DO.redLed);
    List.addData(12, DO.ChiFa_Switch);
    List.addData(13, DO.QacSplit);
    db.update(7, DI.Load_Switch, tableNameLogic);
    db.update(9, DI.PV_Switch, tableNameLogic);
    db.update(6, DI.ChiFa_Switch, tableNameLogic);
    db.update(451, DI.ChiFa_Singn, tableNameLogic);
    db.insert(List.spliceData(tableNameIODO));

    auto now = std::chrono::steady_clock::now();
    if (lastHistoryTime == std::chrono::steady_clock::time_point{}
        || now - lastHistoryTime >= std::chrono::seconds(30)) {
        iodoHistory();
        lastHistoryTime = now;
    }
}


void Dido::iodoHistory()
{
    listhistory.clearData();
    listhistory.addData("ChiFa_Switch_DI", DI.ChiFa_Switch);
    listhistory.addData("PV_Switch_DI", DI.PV_Switch);
    listhistory.addData("Load_Switch_DI", DI.Load_Switch);
    listhistory.addData("ChiFa_Singn_DI", DI.ChiFa_Singn);
    listhistory.addData("YellowLight", DO.yellowLed);
    listhistory.addData("GreenLight", DO.greenLed);
    listhistory.addData("RedLight", DO.redLed);
    listhistory.addData("ChiFa_Switch_DO", DO.ChiFa_Switch);
    listhistory.addData("QacSplit", DO.QacSplit);
    DB.insert(listhistory.spliceData(tableNameIODO));
}


void Dido::didoThread(MySQLConnectionPool& pool)
{
    while (true)
    {
        try
        {
            handleExternalData(pool);
            staterun(commErrCount);
            ioDoData(pool);
        }
        catch (const std::exception& e)
        {
            std::cerr << "didoThread 异常: " << e.what() << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(DIDO_THREAD_SLEEP_MS));
    }
}

