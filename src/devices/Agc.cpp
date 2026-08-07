#include "Agc.h"
#include "Config.h"
#include <chrono>
#include <iostream>
#include <thread>

Agc::Agc(const char* serial_port, int baud_rate)
    : modbusclient(serial_port, baud_rate)
{
}

Agc::~Agc()
{
    modbusclient.disconnect();
}

bool Agc::readAndWriteData(MySQLConnectionPool& pool, int slaveID, int amount)
{
    const int invIdx = amount - 1;
    if (invIdx < 0 || static_cast<std::size_t>(invIdx) >= kAgcMaxGensets)
        return false;

    MySQLDatabase db(pool);
    const int comAlarmAddr = kComAlarmAgcCommAddrBase + amount - 1;
    const std::string tableName = tableNameAGC + std::to_string(amount);

    // 通信/读取失败处理：累计失败次数，超过阈值置告警标志，本次不写库
    auto failCycle = [&]() -> bool {
        commErrCount[invIdx]++;
        if (commErrCount[invIdx] > 3) {
            commErrflg[invIdx] = 1;
        }
        db.update(0, commErrflg[invIdx], tableName);
        db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");
        return false;
    };

    const bool commOk = modbusclient.connect(slaveID)
        && modbusclient.commTest(500, 1, arr_uint16, tableName) != -1;

    if (!commOk) {
        return failCycle();
    }

    bool readOk = true;

    if (modbusclient.readInputRegisters(501, 9, arr_uint16) != -1) {       
        agc_data[invIdx].ABVoltage = arr_uint16[0];
        agc_data[invIdx].BCVoltage = arr_uint16[1];
        agc_data[invIdx].CAVoltage = arr_uint16[2];
        agc_data[invIdx].APhaseVoltage = arr_uint16[3];
        agc_data[invIdx].BPhaseVoltage = arr_uint16[4];
        agc_data[invIdx].CPhaseVoltage = arr_uint16[5];
        agc_data[invIdx].AFrequency = static_cast<int16_t>(arr_uint16[6]);
        agc_data[invIdx].BFrequency = static_cast<int16_t>(arr_uint16[7]);
        agc_data[invIdx].CFrequency = static_cast<int16_t>(arr_uint16[8]);
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(513, 17, arr_uint16) != -1) {
        agc_data[invIdx].APhaseCurrent = static_cast<int16_t>(arr_uint16[0]);
        agc_data[invIdx].BPhaseCurrent = static_cast<int16_t>(arr_uint16[1]);
        agc_data[invIdx].CPhaseCurrent = static_cast<int16_t>(arr_uint16[2]);
        agc_data[invIdx].APhaseActivePower = static_cast<int16_t>(arr_uint16[3]);
        agc_data[invIdx].BPhaseActivePower = static_cast<int16_t>(arr_uint16[4]);
        agc_data[invIdx].CPhaseActivePower = static_cast<int16_t>(arr_uint16[5]);
        agc_data[invIdx].TotalActivePower = static_cast<int16_t>(arr_uint16[6]);
        agc_data[invIdx].APhaseReactivePower = static_cast<int16_t>(arr_uint16[7]);
        agc_data[invIdx].BPhaseReactivePower = static_cast<int16_t>(arr_uint16[8]);
        agc_data[invIdx].CPhaseReactivePower = static_cast<int16_t>(arr_uint16[9]);
        agc_data[invIdx].TotalReactivePower = static_cast<int16_t>(arr_uint16[10]);
        agc_data[invIdx].APhaseApparentPower = static_cast<int16_t>(arr_uint16[11]);
        agc_data[invIdx].BPhaseApparentPower = static_cast<int16_t>(arr_uint16[12]);
        agc_data[invIdx].CPhaseApparentPower = static_cast<int16_t>(arr_uint16[13]);
        agc_data[invIdx].TotalApparentPower = static_cast<int16_t>(arr_uint16[14]);
        agc_data[invIdx].TotalReactiveEnergy = (static_cast<uint32_t>(arr_uint16[15]) << 16) | arr_uint16[16];
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(536, 2, arr_uint16) != -1) {
        agc_data[invIdx].TotalActiveEnergy =
            (static_cast<uint32_t>(arr_uint16[0]) << 16) | arr_uint16[1];
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(538, 1, arr_uint16) != -1) {
        agc_data[invIdx].TotalPowerFactor = static_cast<int16_t>(arr_uint16[0]);
    } else {
        readOk = false;
    }

    if (modbusclient.readInputRegisters(615, 1, arr_uint16) != -1) {
        agc_data[invIdx].EngineOilLevel = static_cast<int16_t>(arr_uint16[0]);
    } else {
        readOk = false;
    }

    // 任一组寄存器读取失败：本轮不写库，避免把上一台机组残留数据写入本表
    if (!readOk) {
        return failCycle();
    }

    // 整轮读取成功：清除累计错误，恢复通信正常标志
    commErrCount[invIdx] = 0;
    commErrflg[invIdx] = 0;
    db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");

    List.clearData();
    List.addData(0, commErrflg[invIdx]);
    List.addData(1, agc_data[invIdx].ABVoltage);
    List.addData(2, agc_data[invIdx].BCVoltage);
    List.addData(3, agc_data[invIdx].CAVoltage);
    List.addData(4, agc_data[invIdx].APhaseVoltage);
    List.addData(5, agc_data[invIdx].BPhaseVoltage);
    List.addData(6, agc_data[invIdx].CPhaseVoltage);
    List.addData(7, agc_data[invIdx].AFrequency * agcRatio_value2);
    List.addData(8, agc_data[invIdx].BFrequency * agcRatio_value2);
    List.addData(9, agc_data[invIdx].CFrequency * agcRatio_value2);
    List.addData(10, agc_data[invIdx].APhaseCurrent);
    List.addData(11, agc_data[invIdx].BPhaseCurrent);
    List.addData(12, agc_data[invIdx].CPhaseCurrent);
    List.addData(13, agc_data[invIdx].APhaseActivePower);
    List.addData(14, agc_data[invIdx].BPhaseActivePower);
    List.addData(15, agc_data[invIdx].CPhaseActivePower);
    List.addData(16, agc_data[invIdx].TotalActivePower);
    List.addData(17, agc_data[invIdx].APhaseReactivePower);
    List.addData(18, agc_data[invIdx].BPhaseReactivePower);
    List.addData(19, agc_data[invIdx].CPhaseReactivePower);
    List.addData(20, agc_data[invIdx].TotalReactivePower);
    List.addData(21, agc_data[invIdx].APhaseApparentPower);
    List.addData(22, agc_data[invIdx].BPhaseApparentPower);
    List.addData(23, agc_data[invIdx].CPhaseApparentPower);
    List.addData(24, agc_data[invIdx].TotalApparentPower);
    List.addData(25, agc_data[invIdx].TotalActiveEnergy);
    List.addData(26, agc_data[invIdx].TotalReactiveEnergy);
    List.addData(27, agc_data[invIdx].TotalPowerFactor * agcRatio_value2);
    List.addData(28, agc_data[invIdx].EngineOilLevel * agcRatio_value1);
    db.insert(List.spliceData(tableName));

    auto now = std::chrono::steady_clock::now();
    if (lastHistoryTime[invIdx] == std::chrono::steady_clock::time_point{}
        || now - lastHistoryTime[invIdx] >= std::chrono::seconds(30)) {
        agcHistory(amount);
        lastHistoryTime[invIdx] = now;
    }

    return true;
}

void Agc::agcHistory(int amount)
{
    const int invIdx = amount - 1;
    if (invIdx < 0 || static_cast<std::size_t>(invIdx) >= kAgcMaxGensets)
        return;

    infList.clearData();
    infList.addData("Online", commErrflg[invIdx]);
    infList.addData("ABVoltage", agc_data[invIdx].ABVoltage);
    infList.addData("BCVoltage", agc_data[invIdx].BCVoltage);
    infList.addData("CAVoltage", agc_data[invIdx].CAVoltage);
    infList.addData("APhaseVoltage", agc_data[invIdx].APhaseVoltage);
    infList.addData("BPhaseVoltage", agc_data[invIdx].BPhaseVoltage);
    infList.addData("CPhaseVoltage", agc_data[invIdx].CPhaseVoltage);
    infList.addData("AFrequency", agc_data[invIdx].AFrequency * agcRatio_value2);
    infList.addData("BFrequency", agc_data[invIdx].BFrequency * agcRatio_value2);
    infList.addData("CFrequency", agc_data[invIdx].CFrequency * agcRatio_value2);
    infList.addData("APhaseCurrent", agc_data[invIdx].APhaseCurrent);
    infList.addData("BPhaseCurrent", agc_data[invIdx].BPhaseCurrent);
    infList.addData("CPhaseCurrent", agc_data[invIdx].CPhaseCurrent);
    infList.addData("APhaseActivePower", agc_data[invIdx].APhaseActivePower);
    infList.addData("BPhaseActivePower", agc_data[invIdx].BPhaseActivePower);
    infList.addData("CPhaseActivePower", agc_data[invIdx].CPhaseActivePower);
    infList.addData("TotalActivePower", agc_data[invIdx].TotalActivePower);
    infList.addData("APhaseReactivePower", agc_data[invIdx].APhaseReactivePower);
    infList.addData("BPhaseReactivePower", agc_data[invIdx].BPhaseReactivePower);
    infList.addData("CPhaseReactivePower", agc_data[invIdx].CPhaseReactivePower);
    infList.addData("TotalReactivePower", agc_data[invIdx].TotalReactivePower);
    infList.addData("APhaseApparentPower", agc_data[invIdx].APhaseApparentPower);
    infList.addData("BPhaseApparentPower", agc_data[invIdx].BPhaseApparentPower);
    infList.addData("CPhaseApparentPower", agc_data[invIdx].CPhaseApparentPower);
    infList.addData("TotalApparentPower", agc_data[invIdx].TotalApparentPower);
    infList.addData("TotalActiveEnergy", agc_data[invIdx].TotalActiveEnergy);
    infList.addData("TotalReactiveEnergy", agc_data[invIdx].TotalReactiveEnergy);
    infList.addData("TotalPowerFactor", agc_data[invIdx].TotalPowerFactor * agcRatio_value2);
    infList.addData("EngineOilLevel", agc_data[invIdx].EngineOilLevel * agcRatio_value1);
    influxDb.insert(infList.spliceData(tableNameAGC + std::to_string(amount)));
}

void Agc::runAgcThread(MySQLConnectionPool& pool){
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cyclePeriod = std::chrono::milliseconds(Config::AGC_DATA_INTERVAL);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            const int rawCount = db.select(kQtAddrAgcGensetCount, "qt");
            int n = rawCount < 0 ? 0 : rawCount;
            if (static_cast<std::size_t>(n) > kAgcMaxGensets)
                n = static_cast<int>(kAgcMaxGensets);

            if (n > 0) {
                for (int i = 1; i <= n; ++i) {
                    readAndWriteData(pool, i, i);
                    if (i < n) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(Config::METER_SLAVE_SWITCH_DELAY_MS));
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "AGC数据线程错误: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "AGC数据线程未知错误" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}
