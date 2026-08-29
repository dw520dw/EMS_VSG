#include "AMC_Meter.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

AMC_Meter::AMC_Meter(const std::string& deviceIdA, const std::string& deviceIdB,
                     const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    ModbusDeviceProfile pa = loadModbusDeviceProfile(path, deviceIdA);
    ModbusDeviceProfile pb = loadModbusDeviceProfile(path, deviceIdB);
    if (pa.rtu_device.empty()) {
        throw std::runtime_error(deviceIdA + ": rtu_device empty in " + path);
    }
    bus_ = std::make_unique<ModbusRTU>(pa.rtu_device.c_str(), pa.rtu_baud, pa.timeout_ms);
    engA_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(pa));
    engB_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(pb));
    attachLogicHook(*engA_);
    attachLogicHook(*engB_);
    std::cout << "[meter] " << deviceIdA << "+" << deviceIdB
              << " rtu=" << engA_->profile().rtu_device << std::endl;
}

void AMC_Meter::attachLogicHook(ModbusPollEngine& eng)
{
    eng.setPostDecodeHook([this](ModbusPollEngine& e, IDataSink& sink) {
        writeLogic(e, static_cast<MySQLDatabase&>(sink));
    });
}

void AMC_Meter::writeLogic(ModbusPollEngine& eng, MySQLDatabase& db)
{
    // 仅成功轮旁路；吸收=充电，释放=放电
    if (eng.commFlag() != 0) {
        return;
    }
    const std::string& id = eng.profile().id;
    const double p = eng.getValue("TotalActivePower");
    const double charge = eng.getValue("AbsorbActiveEnergyPrimary");
    const double discharge = eng.getValue("ReleaseActiveEnergyPrimary");
    if (id == Config::LOAD_METER_DEVICE_ID) {
        db.update(501, eng.commFlag(), "logic");
        db.update(502, p, "logic");
    } else if (id == Config::DG_METER_DEVICE_ID) {
        db.update(452, p, "logic");
         db.update(302, p, "logic");
    } else if (id == Config::PV_METER_DEVICE_ID) {
        db.update(153, p, "logic");
        db.update(18, charge, "data_total");
    } else if (id == Config::ESS_METER_DEVICE_ID) {
        db.update(21, charge, "data_total");
        db.update(22, discharge, "data_total");
    }
}

AMC_Meter::~AMC_Meter()
{
    try {
        if (bus_) {
            bus_->disconnect();
        }
    } catch (...) {
    }
}

void AMC_Meter::runPairThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int pollMs = engA_->profile().poll_ms > 0 ? engA_->profile().poll_ms : 600;
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            engA_->pollOnce(db);
            engB_->pollOnce(db);
        } catch (const std::exception& e) {
            std::cerr << "[meter] " << e.what() << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < std::chrono::milliseconds(pollMs)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(pollMs) - elapsed);
        }
    }
}
