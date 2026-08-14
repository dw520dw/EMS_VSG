#include "Dido.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

Dido::Dido(const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    ModbusDeviceProfile p = loadModbusDeviceProfile(path, Config::DIDO_DEVICE_ID);
    if (p.tcp_ip.empty()) {
        throw std::runtime_error("dido: tcp_ip empty in " + path);
    }
    bus_ = std::make_unique<ModbusTCP>(p.tcp_ip, p.tcp_port, p.slave, p.timeout_ms);
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(p));
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterDecode(eng, static_cast<MySQLDatabase&>(sink));
    });
    std::cout << "[dido] tcp=" << engine_->profile().tcp_ip << ":" << engine_->profile().tcp_port
              << std::endl;
}

Dido::~Dido()
{
    if (bus_) {
        bus_->disconnect();
    }
}

void Dido::writeDoIfChanged(int reg, bool desired, bool& actual, const char* name)
{
    if (desired == actual) {
        return;
    }
    const uint16_t val = desired ? 1 : 0;
    if (bus_->writeRegister(reg, val, engine_->profile().resolvedMysqlTable()) == -1) {
        return;
    }
    actual = desired;
    LOG_ACTION(std::to_string(reg) + (val ? "使能" : "关闭") + name);
}

void Dido::onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db)
{
    greenLed_ = db.select(101, "data_total") != 0;
    yellowLed_ = db.select(102, "data_total") != 0;
    redLed_ = db.select(103, "data_total") != 0;
    chiFaSwitch_ = db.select(107, "data_total") != 0;
    qacSplit_ = db.select(116, "data_total") != 0;

    if (bus_->readRegisters(200, 5, arr_) != -1) {
        readYellow_ = arr_[0] != 0;
        readGreen_ = arr_[1] != 0;
        readRed_ = arr_[2] != 0;
        readChiFa_ = arr_[3] != 0;
        readQac_ = arr_[4] != 0;
        writeDoIfChanged(200, yellowLed_, readYellow_, "黄灯");
        writeDoIfChanged(201, greenLed_, readGreen_, "绿灯");
        writeDoIfChanged(202, redLed_, readRed_, "红灯");
        writeDoIfChanged(203, chiFaSwitch_, readChiFa_, "柴发开关");
        writeDoIfChanged(204, qacSplit_, readQac_, "总开关");
    }

    eng.setValue("YellowLight", yellowLed_ ? 1.0 : 0.0);
    eng.setValue("GreenLight", greenLed_ ? 1.0 : 0.0);
    eng.setValue("RedLight", redLed_ ? 1.0 : 0.0);
    eng.setValue("ChiFa_Switch_DO", chiFaSwitch_ ? 1.0 : 0.0);
    eng.setValue("QacSplit", qacSplit_ ? 1.0 : 0.0);

    const int chiFa = eng.getValue("ChiFa_Switch_DI") > 0.5 ? 1 : 0;
    const int pv = eng.getValue("PV_Switch_DI") > 0.5 ? 1 : 0;
    const int load = eng.getValue("Load_Switch_DI") > 0.5 ? 1 : 0;
    const int sign = eng.getValue("ChiFa_Singn_DI") > 0.5 ? 1 : 0;
    db.update(6, chiFa, "logic");
    db.update(9, pv, "logic");
    db.update(7, load, "logic");
    db.update(451, sign, "logic");
}

void Dido::didoThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 500;
    const auto period = std::chrono::milliseconds(pollMs);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            engine_->pollOnce(db);
        } catch (const std::exception& e) {
            std::cerr << "[dido] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[dido] unknown error" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}
