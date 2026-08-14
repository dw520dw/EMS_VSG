#include "Bamu.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

inline bool alarmOn(const ModbusPollEngine& eng, const char* name)
{
    return eng.getValue(name) > 0.5;
}

}  // namespace

Bamu::Bamu(const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    auto load = [&](const char* id) { return loadModbusDeviceProfile(path, id); };

    ModbusDeviceProfile stackProfile = load(Config::BAMU_STACK_DEVICE_ID);
    if (stackProfile.tcp_ip.empty()) {
        throw std::runtime_error("bamu_stack: tcp_ip empty in " + path);
    }
    bus_ = std::make_unique<ModbusTCP>(stackProfile.tcp_ip, stackProfile.tcp_port,
                                       stackProfile.slave, stackProfile.timeout_ms);

    stackEngine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(stackProfile));
    stackEngine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterStack(eng, static_cast<MySQLDatabase&>(sink));
    });

    bmsEngine_ = std::make_unique<ModbusPollEngine>(*bus_, load(Config::BAMU_BMS_DEVICE_ID));
    bmsEngine_->setSkipProbe(true);
    bmsEngine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterBms(eng, static_cast<MySQLDatabase&>(sink));
    });

    airEngine_ = std::make_unique<ModbusPollEngine>(*bus_, load(Config::BAMU_AIR_DEVICE_ID));
    airEngine_->setSkipProbe(true);
    airEngine_->setInheritCommFlag(0);
    airEngine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterAir(eng, static_cast<MySQLDatabase&>(sink));
    });

    std::cout << "[bamu] loaded stack/bms/air engines" << std::endl;
}

Bamu::~Bamu()
{
    if (bus_) {
        bus_->disconnect();
    }
}

void Bamu::flushDbList(MySQLDatabase& db, const std::string& table)
{
    db.insert(dbList_.spliceData(table));
}

int Bamu::computeAlarmLevel(const ModbusPollEngine& eng) const
{
    if (alarmOn(eng, "ModuleOverVolAlarm3") || alarmOn(eng, "ModuleUnderVolAlarm3") ||
        alarmOn(eng, "ModuleOverTempAlarm3") || alarmOn(eng, "ModuleUnderTempAlarm3") ||
        alarmOn(eng, "CellOverVolAlarm3") || alarmOn(eng, "CellUnderVolAlarm3") ||
        alarmOn(eng, "CellOverTempAlarm3") || alarmOn(eng, "CellUnderTempAlarm3") ||
        alarmOn(eng, "SOCLowAlarm3") || alarmOn(eng, "SOCHighAlarm3") ||
        alarmOn(eng, "SOHLowAlarm3") || alarmOn(eng, "GroupOverTempAlarm3") ||
        alarmOn(eng, "GroupUnderVolAlarm3") || alarmOn(eng, "GroupOverVolAlarm3") ||
        alarmOn(eng, "GroupOverCurrAlarm3") || alarmOn(eng, "InsulationLowAlarm3") ||
        alarmOn(eng, "CellVolDiffAlarm3") || alarmOn(eng, "CellTempDiffAlarm3") ||
        alarmOn(eng, "SlaveDisconnectAlarm") || alarmOn(eng, "MasterDisconnectAlarm") ||
        alarmOn(eng, "GroupVolAbnormalAlarm") || alarmOn(eng, "ContactorBreakAbnormalAlarm") ||
        alarmOn(eng, "ContactorCloseAbnormalAlarm") ||
        alarmOn(eng, "VolCollectDisconnectAlarm") || alarmOn(eng, "CurrCollectDisconnectAlarm") ||
        alarmOn(eng, "Fault")) {
        return 3;
    }
    if (alarmOn(eng, "ModuleOverVolAlarm2") || alarmOn(eng, "ModuleUnderVolAlarm2") ||
        alarmOn(eng, "ModuleOverTempAlarm2") || alarmOn(eng, "ModuleUnderTempAlarm2") ||
        alarmOn(eng, "CellOverVolAlarm2") || alarmOn(eng, "CellUnderVolAlarm2") ||
        alarmOn(eng, "CellOverTempAlarm2") || alarmOn(eng, "CellUnderTempAlarm2") ||
        alarmOn(eng, "SOCLowAlarm2") || alarmOn(eng, "SOCHighAlarm2") ||
        alarmOn(eng, "SOHLowAlarm2") || alarmOn(eng, "GroupOverTempAlarm2") ||
        alarmOn(eng, "GroupUnderVolAlarm2") || alarmOn(eng, "GroupOverVolAlarm2") ||
        alarmOn(eng, "GroupOverCurrAlarm2") || alarmOn(eng, "InsulationLowAlarm2") ||
        alarmOn(eng, "CellVolDiffAlarm2") || alarmOn(eng, "ChargeStopAlarm") ||
        alarmOn(eng, "DischargeStopAlarm") || alarmOn(eng, "CellTempDiffAlarm2")) {
        return 2;
    }
    if (alarmOn(eng, "ModuleOverVolAlarm1") || alarmOn(eng, "ModuleUnderVolAlarm1") ||
        alarmOn(eng, "ModuleOverTempAlarm1") || alarmOn(eng, "ModuleUnderTempAlarm1") ||
        alarmOn(eng, "CellOverVolAlarm1") || alarmOn(eng, "CellUnderVolAlarm1") ||
        alarmOn(eng, "CellOverTempAlarm1") || alarmOn(eng, "CellUnderTempAlarm1") ||
        alarmOn(eng, "SOCLowAlarm1") || alarmOn(eng, "SOCHighAlarm1") ||
        alarmOn(eng, "SOHLowAlarm1") || alarmOn(eng, "GroupOverTempAlarm1") ||
        alarmOn(eng, "GroupUnderVolAlarm1") || alarmOn(eng, "GroupOverVolAlarm1") ||
        alarmOn(eng, "GroupOverCurrAlarm1") || alarmOn(eng, "InsulationLowAlarm1") ||
        alarmOn(eng, "CellVolDiffAlarm1") || alarmOn(eng, "CellTempDiffAlarm1")) {
        return 1;
    }
    return 0;
}

void Bamu::onAfterStack(ModbusPollEngine& eng, MySQLDatabase& db)
{
    const double v = eng.getValue("TotalVoltage");
    const double i = eng.getValue("TotalCurrent");
    eng.setValue("ActivePower", v * i * BamuRatio_Value3);
    eng.setValue("AlarmLevel", static_cast<double>(computeAlarmLevel(eng)));

    // VSG：data_total.112=1 → 写 503=2 停机
    if (db.select(112, "data_total")) {
        bus_->writeRegister(503, 2, "bamu");
        LOG_ACTION("BAMU系统停止, 写入503寄存器, 值为2");
    }
}

void Bamu::onAfterBms(ModbusPollEngine& eng, MySQLDatabase& /*db*/)
{
    const double v = eng.getValue("TotalVoltage");
    const double i = eng.getValue("TotalCurrent");
    eng.setValue("Power", v * i * BamuRatio_Value3);
}

void Bamu::onAfterAir(ModbusPollEngine& eng, MySQLDatabase& db)
{
    eng.setValue("FanStatus", eng.getValue("_fan_speed") > 5.0 ? 1.0 : 0.0);
    eng.setValue("WaterPumpStatus", eng.getValue("_pump_speed") > 5.0 ? 1.0 : 0.0);
    eng.setValue("Compressor", eng.getValue("_comp_speed") > 5.0 ? 1.0 : 0.0);

    const int id = eng.profile().table_suffix;
    if (isValidAirUnitId(id)) {
        airOnline_[static_cast<size_t>(id)] = eng.getValue("Online");
        db.update(airUnitToComAlarmAddr(id),
                  static_cast<int>(eng.getValue("Online") > 0.5 ? 1 : 0), "com_alarm");
    }
}

bool Bamu::readDiSignals(DiSignals& out)
{
    if (bus_->readInputBits(DI_START_ADDR, DI_COUNT, arr_input_bits_) != -1) {
        out.EPO = arr_input_bits_[1] != 0;
        out.TotalSwitch = arr_input_bits_[2] != 0;
        out.FireLevel1 = arr_input_bits_[4] != 0;
        out.FireControl = arr_input_bits_[5] != 0;
        return true;
    }
    return false;
}

void Bamu::writeLogicData(MySQLDatabase& db, const ModbusPollEngine& stack)
{
    dbList_.clearData();
    const int clusterCount = clampBmsClusterCount(batteryNumber_);
    const int airUnitCount = bmsClusterToAirUnitId(clusterCount);
    for (int airUnitId = 1; airUnitId <= airUnitCount; ++airUnitId) {
        dbList_.addData(airUnitToLogicCommAddr(airUnitId),
                        static_cast<int>(airOnline_[static_cast<size_t>(airUnitId)] > 0.5 ? 1 : 0));
    }
    dbList_.addData(401, stack.commFlag());
    dbList_.addData(402, stack.getValue("SOC"));
    dbList_.addData(403, stack.getValue("MaxChargePower"));
    dbList_.addData(404, stack.getValue("MaxDischargePower"));
    dbList_.addData(405, stack.getValue("AlarmLevel"));
    dbList_.addData(406, static_cast<int>(diSignals_.EPO));
    dbList_.addData(411, stack.getValue("ActivePower"));
    dbList_.addData(412, static_cast<int>(diSignals_.FireLevel1));
    dbList_.addData(413, static_cast<int>(diSignals_.FireControl));
    dbList_.addData(10, static_cast<int>(diSignals_.TotalSwitch));
    flushDbList(db, "logic");

    dbList_.clearData();
    dbList_.addData(4, static_cast<int>(diSignals_.EPO));
    dbList_.addData(5, static_cast<int>(diSignals_.FireLevel1));
    dbList_.addData(6, static_cast<int>(diSignals_.FireControl));
    dbList_.addData(7, static_cast<int>(diSignals_.TotalSwitch));
    flushDbList(db, "dodi");
}

void Bamu::readBmsCellData(int id)
{
    const int addressOffset = (id - 1) * 3000;
    if (bus_->readInputRegisters(191 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i] = arr_input_registers_[i];
        }
    }
    if (bus_->readInputRegisters(311 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i + BMS_CELL_MODBUS_BLOCK] =
                arr_input_registers_[i];
        }
    }
    if (bus_->readInputRegisters(431 + addressOffset, BMS_CELL_MODBUS_BLOCK2, arr_input_registers_) !=
        -1) {
        const int base = BMS_CELL_MODBUS_BLOCK * 2;
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK2; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[base + i] = arr_input_registers_[i];
        }
    }
    if (bus_->readInputRegisters(891 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Temperature[i] =
                static_cast<uint16_t>(arr_input_registers_[i] - 40);
        }
    }
    if (bus_->readInputRegisters(1011 + addressOffset, BMS_CELL_MODBUS_BLOCK1,
                                 arr_input_registers_) != -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK1; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Temperature[BMS_CELL_MODBUS_BLOCK + i] =
                static_cast<uint16_t>(arr_input_registers_[i] - 40);
        }
    }
}

void Bamu::writeCellData(MySQLDatabase& db, int id)
{
    auto& lastVol = lastVolDb_[static_cast<size_t>(id)];
    auto& lastTemp = lastTempDb_[static_cast<size_t>(id)];
    bool& hasVol = hasVolSnap_[static_cast<size_t>(id)];
    bool& hasTemp = hasTempSnap_[static_cast<size_t>(id)];

    dbList_.clearData();
    for (int i = 0; i < BMS_CELL_VOLTAGE_COUNT; ++i) {
        addDbDataIfChanged(dbList_, i + 1,
                           bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i] * BamuRatio_Value3,
                           lastVol, hasVol);
    }
    hasVol = true;
    flushDbList(db, "bms_cellvol" + std::to_string(id));

    dbList_.clearData();
    for (int i = 0; i < BMS_CELL_TEMPERATURE_COUNT; ++i) {
        addDbDataIfChanged(dbList_, i + 1,
                           static_cast<double>(bmsCell_[static_cast<size_t>(id)].Cell_Temperature[i]),
                           lastTemp, hasTemp);
    }
    hasTemp = true;
    flushDbList(db, "bms_celltemp" + std::to_string(id));
}

void Bamu::processStack(MySQLDatabase& db)
{
    if (++qtPollCounter_ >= Config::BAMU_QT_CONFIG_POLL_INTERVAL || batteryNumber_ < 1) {
        qtPollCounter_ = 0;
        batteryNumber_ = db.select(stackEngine_->profile().count_qt_addr, "qt");
    }
    stackEngine_->pollOnce(db);
    readDiSignals(diSignals_);
    writeLogicData(db, *stackEngine_);
}

void Bamu::processCluster(MySQLDatabase& db, int id)
{
    if (stackEngine_->commFlag() != 0) {
        bmsEngine_->setTableSuffix(id);
        bmsEngine_->setInheritCommFlag(1);
        bmsEngine_->setSkipProbe(true);
        bmsEngine_->pollOnce(db);
        return;
    }

    bmsEngine_->setTableSuffix(id);
    bmsEngine_->setClusterIndex(id);
    bmsEngine_->setSkipProbe(true);
    bmsEngine_->setInheritCommFlag(0);
    bmsEngine_->pollOnce(db);

    if (isAirUnitLeadCluster(id)) {
        const int airUnitId = bmsClusterToAirUnitId(id);
        airEngine_->setTableSuffix(airUnitId);
        airEngine_->setClusterIndex(airUnitId);
        airEngine_->setSkipProbe(true);
        airEngine_->setInheritCommFlag(0);
        airEngine_->pollOnce(db);
    }

    const auto now = std::chrono::steady_clock::now();
    if (lastCellRealtime_[static_cast<size_t>(id)] == std::chrono::steady_clock::time_point{} ||
        now - lastCellRealtime_[static_cast<size_t>(id)] >=
            std::chrono::milliseconds(Config::BMS_CELL_REALTIME_INTERVAL)) {
        readBmsCellData(id);
        writeCellData(db, id);
        lastCellRealtime_[static_cast<size_t>(id)] = now;
    }
}

void Bamu::runBamuDataThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto period = std::chrono::milliseconds(Config::BAMU_DATA_INTERVAL);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            processStack(db);
            const int n = clampBmsClusterCount(batteryNumber_);
            for (int i = 1; i <= n; ++i) {
                processCluster(db, i);
            }
        } catch (const std::exception& e) {
            std::cerr << "[bamu] " << e.what() << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}
