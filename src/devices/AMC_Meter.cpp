#include "AMC_Meter.h"
#include "Config.h"
#include <chrono>
#include <iostream>
#include <thread>



namespace {
// 根据电表类型返回对应的表名
const std::string& meterTableName(int amount,
                                 const std::string& load,
                                 const std::string& dg,
                                 const std::string& pv,
                                 const std::string& ess)
{
    switch (amount) {
    case 1: return load;
    case 2: return dg;
    case 3: return pv;
    case 4: return ess;
    default: return load;
    }
}

// 电表数据上下文
struct MeterFieldContext {
    const STR_Mater_Data& data;
    int commFlag;
};

// 电表字段映射表
struct MeterFieldMapEntry {
    int dbAddr;
    const char* influxKey;
    double (*getter)(const MeterFieldContext&);
};

// 电表字段映射表宏定义
#define METER_FIELD_COMM(addr, key) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.commFlag); } }
#define METER_FIELD_VOLT(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.data.field) * meterRatio_value1 * c.data.Pt; } }
#define METER_FIELD_CURR(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.data.field) * meterRatio_value3 * c.data.Ct; } }
#define METER_FIELD_POWER(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.data.field) * meterRatio_value3 * c.data.Ct * c.data.Pt; } }
#define METER_FIELD_PF(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.data.field) * meterRatio_value3; } }
#define METER_FIELD_FREQ(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return static_cast<double>(c.data.field) * meterRatio_value2; } }
#define METER_FIELD_DOUBLE(addr, key, field) \
    { addr, key, [](const MeterFieldContext& c) -> double { return c.data.field; } }

// 电表字段映射表
static const MeterFieldMapEntry kMeterFieldMaps[] = {
#include "AMC_Meter_field_maps.inc"
};

// 电表字段映射表数量
static constexpr size_t kMeterFieldMapCount = sizeof(kMeterFieldMaps) / sizeof(MeterFieldMapEntry);
static_assert(kMeterFieldMapCount == 29, "Meter field map count mismatch");

} // namespace
// 电表寄存器读取延时
inline void meterRegisterReadDelay()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(Config::METER_REGISTER_READ_DELAY_MS));
}

AMC_Meter::AMC_Meter(const char* serial_port, int baud_rate)
    : modbusclient(serial_port, baud_rate)
{
}

AMC_Meter::~AMC_Meter()
{
    try {
        modbusclient.disconnect();
    } catch (...) {
    }
}
//读取电表数据并写入数据库
bool AMC_Meter::readAndWriteData(MySQLConnectionPool& pool, int slaveID, int amount)
{
    const int invIdx = amount - 1;
    if (invIdx < 0 || static_cast<std::size_t>(invIdx) >= kMeterLogicalSlotCount)
        return false;

    MySQLDatabase db(pool);
    const int comAlarmAddr = kComAlarmMeterCommAddrBase + amount - 1;
    const std::string& tableName =
        meterTableName(amount, tableNameLoadMeter, tableNameDgMeter, tableNamePvMeter, tableNameEssMeter);

    // 通信/读取失败处理：累计失败次数，超过阈值置告警标志，本次不写库
    auto failCycle = [&]() -> bool {
        commErrCount[invIdx]++;
        if (commErrCount[invIdx] > 3) {
            commErrflg[invIdx] = 1;
            db.update(0, commErrflg[invIdx], tableName);
        }
        db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");
        return false;
    };

    const bool commOk = modbusclient.connect(slaveID)
        && modbusclient.commTest(0x03, 1, arr_uint16, tableNameMeter + std::to_string(amount)) != -1;

    if (!commOk) {
        return failCycle();
    }

    bool readOk = true;

    meterRegisterReadDelay();

    if (modbusclient.readRegisters(0x03, 2, arr_uint16) != -1) {
        meter_data[invIdx].Pt = arr_uint16[0];
        meter_data[invIdx].Ct = arr_uint16[1];
    } else {
        readOk = false;
    }
    meterRegisterReadDelay();

    if (modbusclient.readRegisters(0x3F, 4, arr_uint16) != -1) {
        meter_data[invIdx].absorbActiveEnergySecondary =
            (static_cast<uint32_t>(arr_uint16[0]) << 16) | arr_uint16[1];
        meter_data[invIdx].releaseActiveEnergySecondary =
            (static_cast<uint32_t>(arr_uint16[2]) << 16) | arr_uint16[3];

        meter_data[invIdx].absorbActiveEnergyPrimary =
            static_cast<double>(meter_data[invIdx].absorbActiveEnergySecondary) / 1000.0
            * static_cast<double>(meter_data[invIdx].Pt) * static_cast<double>(meter_data[invIdx].Ct);
        meter_data[invIdx].releaseActiveEnergyPrimary =
            static_cast<double>(meter_data[invIdx].releaseActiveEnergySecondary) / 1000.0
            * static_cast<double>(meter_data[invIdx].Pt) * static_cast<double>(meter_data[invIdx].Ct);
    } else {
        readOk = false;
    }
    meterRegisterReadDelay();

    if (modbusclient.readRegisters(0x23, 2, arr_uint16) != -1) {
        meter_data[invIdx].voltageDecimalPointDPT = static_cast<uint8_t>((arr_uint16[0] >> 8) & 0xFF);
        meter_data[invIdx].currentDecimalPointDCT = static_cast<uint8_t>(arr_uint16[0] & 0xFF);
        meter_data[invIdx].powerDecimalPointDPQ = static_cast<uint8_t>((arr_uint16[1] >> 8) & 0xFF);
        meter_data[invIdx].pqSign = static_cast<uint8_t>(arr_uint16[1] & 0xFF);

        meter_data[invIdx].qNegative = ((meter_data[invIdx].pqSign >> 7) & 0x01) != 0;
        meter_data[invIdx].qcNegative = ((meter_data[invIdx].pqSign >> 6) & 0x01) != 0;
        meter_data[invIdx].qbNegative = ((meter_data[invIdx].pqSign >> 5) & 0x01) != 0;
        meter_data[invIdx].qaNegative = ((meter_data[invIdx].pqSign >> 4) & 0x01) != 0;
        meter_data[invIdx].pNegative = ((meter_data[invIdx].pqSign >> 3) & 0x01) != 0;
        meter_data[invIdx].pcNegative = ((meter_data[invIdx].pqSign >> 2) & 0x01) != 0;
        meter_data[invIdx].pbNegative = ((meter_data[invIdx].pqSign >> 1) & 0x01) != 0;
        meter_data[invIdx].paNegative = (meter_data[invIdx].pqSign & 0x01) != 0;
    } else {
        readOk = false;
    }
    meterRegisterReadDelay();

    if (modbusclient.readRegisters(0x100, 26, arr_uint16) != -1) {
        meter_data[invIdx].APhaseVoltage = arr_uint16[0];
        meter_data[invIdx].BPhaseVoltage = arr_uint16[1];
        meter_data[invIdx].CPhaseVoltage = arr_uint16[2];
        meter_data[invIdx].ABVoltage = arr_uint16[3];
        meter_data[invIdx].BCVoltage = arr_uint16[4];
        meter_data[invIdx].CAVoltage = arr_uint16[5];
        meter_data[invIdx].APhaseCurrent = static_cast<int16_t>(arr_uint16[6]);
        meter_data[invIdx].BPhaseCurrent = static_cast<int16_t>(arr_uint16[7]);
        meter_data[invIdx].CPhaseCurrent = static_cast<int16_t>(arr_uint16[8]);
        meter_data[invIdx].APhaseActivePower = static_cast<int16_t>(arr_uint16[9]);
        meter_data[invIdx].BPhaseActivePower = static_cast<int16_t>(arr_uint16[10]);
        meter_data[invIdx].CPhaseActivePower = static_cast<int16_t>(arr_uint16[11]);
        meter_data[invIdx].TotalActivePower = static_cast<int16_t>(arr_uint16[12]);
        meter_data[invIdx].APhaseReactivePower = static_cast<int16_t>(arr_uint16[13]);
        meter_data[invIdx].BPhaseReactivePower = static_cast<int16_t>(arr_uint16[14]);
        meter_data[invIdx].CPhaseReactivePower = static_cast<int16_t>(arr_uint16[15]);
        meter_data[invIdx].TotalReactivePower = static_cast<int16_t>(arr_uint16[16]);
        meter_data[invIdx].APowerFactor = static_cast<int16_t>(arr_uint16[17]);
        meter_data[invIdx].BPowerFactor = static_cast<int16_t>(arr_uint16[18]);
        meter_data[invIdx].CPowerFactor = static_cast<int16_t>(arr_uint16[19]);
        meter_data[invIdx].TotalPowerFactor = static_cast<int16_t>(arr_uint16[20]);
        meter_data[invIdx].APhaseApparentPower = static_cast<int32_t>(arr_uint16[21]);
        meter_data[invIdx].BPhaseApparentPower = static_cast<int32_t>(arr_uint16[22]);
        meter_data[invIdx].CPhaseApparentPower = static_cast<int32_t>(arr_uint16[23]);
        meter_data[invIdx].TotalApparentPower = static_cast<int32_t>(arr_uint16[24]);
        meter_data[invIdx].Frequency = arr_uint16[25];
    } else {
        readOk = false;
    }

    // 任一组寄存器读取失败：本轮不写库，避免把上一台设备残留数据写入本表
    if (!readOk) {
        return failCycle();
    }

    // 整轮读取成功：清除累计错误，恢复通信正常标志
    commErrCount[invIdx] = 0;
    commErrflg[invIdx] = 0;
    db.update(comAlarmAddr, commErrflg[invIdx], "com_alarm");

    const MeterFieldContext ctx{meter_data[invIdx], commErrflg[invIdx]};
    const double totalActivePower = kMeterFieldMaps[13].getter(ctx);

    List.clearData();
    for (const auto& field : kMeterFieldMaps) {
        List.addData(field.dbAddr, field.getter(ctx));
    }
    db.insert(List.spliceData(tableName));
    if (amount == 1) {
        db.update(501, commErrflg[invIdx], "logic");
        db.update(502, totalActivePower, "logic");
    } else if (amount == 2) {
        db.update(452, totalActivePower, "logic");
    } else if (amount == 3) {
        db.update(153, totalActivePower, "logic");
    }

    auto now = std::chrono::steady_clock::now();
    if (lastHistoryTime[invIdx] == std::chrono::steady_clock::time_point{}
        || now - lastHistoryTime[invIdx] >= std::chrono::seconds(30)) {
        meterHistory(amount);
        lastHistoryTime[invIdx] = now;
    }
    return true;
}
//10秒写一次历史数据
void AMC_Meter::meterHistory(int amount)
{
    const int invIdx = amount - 1;
    if (invIdx < 0 || static_cast<std::size_t>(invIdx) >= kMeterLogicalSlotCount)
        return;

    const std::string& tableName =
        meterTableName(amount, tableNameLoadMeter, tableNameDgMeter, tableNamePvMeter, tableNameEssMeter);

    const MeterFieldContext ctx{meter_data[invIdx], commErrflg[invIdx]};
    infList.clearData();
    for (const auto& field : kMeterFieldMaps) {
        infList.addData(field.influxKey, field.getter(ctx));
    }
    meterDB.insert(infList.spliceData(tableName));
}

void AMC_Meter::runLoadDgThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(10));
    const auto cyclePeriod = std::chrono::milliseconds(Config::METER_DATA_INTERVAL);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            readAndWriteData(pool, 1, 1); // 负载电表
            std::this_thread::sleep_for(std::chrono::milliseconds(Config::METER_SLAVE_SWITCH_DELAY_MS));
            readAndWriteData(pool, 2, 2); // 柴发电表
        } catch (const std::exception& e) {
            std::cerr << "AMC负载/柴发电表线程错误: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "AMC负载/柴发电表线程未知错误" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}

void AMC_Meter::runPvEssThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto cyclePeriod = std::chrono::milliseconds(Config::METER_DATA_INTERVAL);
    while (true) {
        const auto cycleStart = std::chrono::steady_clock::now();
        try {
            readAndWriteData(pool, 1, 3); // 光伏电表
            std::this_thread::sleep_for(std::chrono::milliseconds(Config::METER_SLAVE_SWITCH_DELAY_MS));
            readAndWriteData(pool, 2, 4); // 储能电表
        } catch (const std::exception& e) {
            std::cerr << "AMC光伏/储能电表线程错误: " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "AMC光伏/储能电表线程未知错误" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - cycleStart;
        if (elapsed < cyclePeriod) {
            std::this_thread::sleep_for(cyclePeriod - elapsed);
        }
    }
}
