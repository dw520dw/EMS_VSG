#include "Agc.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

Agc::Agc(const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    ModbusDeviceProfile p = loadModbusDeviceProfile(path, Config::AGC_DEVICE_ID);
    if (p.rtu_device.empty()) {
        throw std::runtime_error("agc: rtu_device empty in " + path);
    }
    bus_ = std::make_unique<ModbusRTU>(p.rtu_device.c_str(), p.rtu_baud, p.timeout_ms);
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(p));
    // 多从站复用同一引擎：探针在 pollOneGenset 逐台处理，引擎内跳过
    engine_->setSkipProbe(true);
    std::cout << "[agc] rtu=" << engine_->profile().rtu_device << std::endl;
}

Agc::~Agc()
{
    if (bus_) {
        bus_->disconnect();
    }
}

void Agc::sleepFrameGap() const
{
    const int ms = engine_->profile().inter_frame_ms;
    if (ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
}

void Agc::pollOneGenset(MySQLDatabase& db, int amount)
{
    const int idx = amount - 1;
    if (idx < 0 || static_cast<std::size_t>(idx) >= kMaxGensets) {
        return;
    }
    engine_->profile().slave = amount;
    engine_->setTableSuffix(amount);
    engine_->profile().comm.com_alarm_addr = kComAlarmAddrBase + idx;
    engine_->clearCaches();
    bus_->setSlave(amount);

    const std::string table = engine_->profile().resolvedMysqlTable();
    const bool ok = bus_->probe(engine_->profile().comm.probe_addr,
                                engine_->profile().comm.probe_count, &probeBuf_, table);
    sleepFrameGap();
    if (!ok) {
        ++commErrCount_[static_cast<std::size_t>(idx)];
        const int offline = commErrCount_[static_cast<std::size_t>(idx)] > 3 ? 1 : 0;
        db.update(0, offline, table);
        db.update(engine_->profile().comm.com_alarm_addr, offline, "com_alarm");
        return;
    }

    commErrCount_[static_cast<std::size_t>(idx)] = 0;
    engine_->setInheritCommFlag(0);
    engine_->pollOnce(db);
}

void Agc::runAgcThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 600;
    const auto period = std::chrono::milliseconds(pollMs);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            int n = db.select(engine_->profile().count_qt_addr, "qt");
            if (n < 0) {
                n = 0;
            }
            if (static_cast<std::size_t>(n) > kMaxGensets) {
                n = static_cast<int>(kMaxGensets);
            }
            for (int i = 1; i <= n; ++i) {
                pollOneGenset(db, i);
                if (i < n) {
                    sleepFrameGap();
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[agc] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[agc] unknown error" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}
