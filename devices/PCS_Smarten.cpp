#include "PCS_Smarten.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

inline void setBitFlag(bool& flag, uint16_t word, int bit)
{
    flag = (word & (1U << bit)) != 0;
}

inline void apply41696ControlBits(SmartenSetData& dst, uint16_t word)
{
    setBitFlag(dst.antiBackflowProtection_41696_bit1, word, 1);
    setBitFlag(dst.powerFactorControl_41696_bit2, word, 2);
    setBitFlag(dst.threePhaseUnbalancedMode_41696_bit3, word, 3);
}

inline uint16_t pack41696ControlWord(bool antiBackflow, bool powerFactor, bool unbalanced)
{
    uint16_t v = 0x0001;
    if (antiBackflow) {
        v |= (1U << 1);
    }
    if (powerFactor) {
        v |= (1U << 2);
    }
    if (unbalanced) {
        v |= (1U << 3);
    }
    return v;
}

}  // namespace

PCS_Smarten::PCS_Smarten(const std::string& configPath)
{
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    ModbusDeviceProfile profile = loadModbusDeviceProfile(path, Config::PCS_SMARTEN_DEVICE_ID);
    if (profile.tcp_ip.empty()) {
        throw std::runtime_error("pcs_smarten: tcp_ip empty in " + path);
    }
    bus_ = std::make_unique<ModbusTCP>(profile.tcp_ip, profile.tcp_port, profile.slave,
                                       profile.timeout_ms);
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(profile));
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterTelemetry(eng, static_cast<MySQLDatabase&>(sink));
    });
    std::cout << "[pcs_smarten] tcp=" << engine_->profile().tcp_ip << ":"
              << engine_->profile().tcp_port << std::endl;
}

PCS_Smarten::~PCS_Smarten()
{
    if (bus_) {
        bus_->disconnect();
    }
}

double PCS_Smarten::readU64Scaled(int startAddr, double scale)
{
    uint16_t w[4] = {};
    if (bus_->readRegisters(startAddr, 4, w) == -1) {
        return 0.0;
    }
    const uint64_t raw = (static_cast<uint64_t>(w[0]) << 48) | (static_cast<uint64_t>(w[1]) << 32) |
                         (static_cast<uint64_t>(w[2]) << 16) | static_cast<uint64_t>(w[3]);
    return static_cast<double>(raw) * scale;
}

void PCS_Smarten::fillVirtual(ModbusPollEngine& eng)
{
    // 与旧逻辑一致：Fault > Alarm > Running(充/放/降额等) > Stop
    const bool fault = eng.getValue("FaultState") > 0.5;
    const bool alarm = eng.getValue("AlarmState") > 0.5;
    const bool isStopLike = eng.getValue("_st41726_b1") > 0.5 || eng.getValue("_st41726_b2") > 0.5 ||
                            eng.getValue("_st41726_b3") > 0.5 || eng.getValue("_st41726_b9") > 0.5 ||
                            eng.getValue("_st41726_b10") > 0.5 || eng.getValue("EmergencyShutdown") > 0.5;
    const bool isRunningLike = eng.getValue("_st41726_b6") > 0.5 || eng.getValue("_st41726_b7") > 0.5 ||
                               eng.getValue("_st41726_b8") > 0.5 || eng.getValue("_st41726_b11") > 0.5 ||
                               eng.getValue("_st41726_b12") > 0.5;

    int status = 0;  // Stop
    if (fault) {
        status = 3;  // Fault
    } else if (alarm) {
        status = 2;  // Alarm
    } else if (isRunningLike) {
        status = 1;  // Running
    } else if (isStopLike) {
        status = 0;
    }
    eng.setValue("Status", static_cast<double>(status));

    const bool onGrid = eng.getValue("_st41726_b4") > 0.5;
    const bool offGrid = eng.getValue("_st41726_b5") > 0.5;
    int grid = 2;  // Unknown
    if (onGrid) {
        grid = 0;
    } else if (offGrid) {
        grid = 1;
    }
    eng.setValue("OnGrid", static_cast<double>(grid));

    eng.setValue("DCDischargeEnergy", readU64Scaled(41845, 0.1));
    eng.setValue("DCChargeEnergy", readU64Scaled(41849, 0.1));
    eng.setValue("ACDischargeEnergy", readU64Scaled(40173, 0.1));
    eng.setValue("ACChargeEnergy", readU64Scaled(40177, 0.1));
}

void PCS_Smarten::writeLogicData(ModbusPollEngine& eng, MySQLDatabase& db)
{
    db.update(101, eng.commFlag(), "logic");
    db.update(102, eng.getValue("Status"), "logic");
    db.update(103, eng.getValue("ACActivePower"), "logic");
    db.update(105, eng.getValue("OnGrid"), "logic");
    db.update(106, eng.getValue("_st41727_b4") > 0.5 ? 1.0 : 0.0, "logic");
}

void PCS_Smarten::dispatchHmiControlCommands(MySQLDatabase& db)
{
    if (bus_->readRegisters(41378, 2, arr_) != -1) {
        deviceSet_.alarmReset_41378 = arr_[0];
        deviceSet_.pcsOnOff_41379 = arr_[1];
    }
    if (bus_->readRegisters(41546, 2, arr_) != -1) {
        deviceSet_.activePowerSetting_41546 = static_cast<int16_t>(arr_[0]);
        deviceSet_.reactivePowerSetting_41547 = static_cast<int16_t>(arr_[1]);
    }
    if (bus_->readRegisters(41671, 1, arr_) != -1) {
        deviceSet_.gridInterMode_41671 = arr_[0];
    }

    hmiSet_.alarmReset_41378 = static_cast<uint16_t>(db.select(602, "qt"));
    static const std::vector<int> kAddrs{108, 109, 113, 115};
    const auto hmiCmd = db.selectMultipleData("data_total", kAddrs);
    hmiSet_.pcsOnOff_41379 = static_cast<uint16_t>(hmiCmd.at(108));
    hmiSet_.gridInterMode_41671 = static_cast<uint16_t>(hmiCmd.at(115));
    const int targetP = static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(109)) * 10));
    const int targetQ = static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(113)) * 10));
    hmiSet_.activePowerSetting_41546 = static_cast<int16_t>(targetP);
    hmiSet_.reactivePowerSetting_41547 = static_cast<int16_t>(targetQ);

    if (hmiSet_.pcsOnOff_41379 == 1 && deviceSet_.pcsOnOff_41379 != 1) {
        LOG_ACTION("PCS开机");
        bus_->writeRegister(41379, 1, "pcsset");
    } else if (hmiSet_.pcsOnOff_41379 == 0 && deviceSet_.pcsOnOff_41379 != 0) {
        LOG_ACTION("PCS关机");
        bus_->writeRegister(41379, 0, "pcsset");
    }

    if (hmiSet_.gridInterMode_41671 == 0 && deviceSet_.gridInterMode_41671 != 0) {
        LOG_ACTION("PCS并网");
        bus_->writeRegister(41671, 0, "pcsset");
    } else if (hmiSet_.gridInterMode_41671 != 0 &&
               deviceSet_.gridInterMode_41671 != hmiSet_.gridInterMode_41671) {
        LOG_ACTION("PCS离网");
        bus_->writeRegister(41671, 1, "pcsset");
    }

    if (hmiSet_.alarmReset_41378 == 1) {
        bus_->writeRegister(41378, 0, "pcsset");
        LOG_ACTION("PCS告警复位");
        db.update(602, 0, "qt");
    }

    constexpr int kDeadband = 10;
    if (std::abs(targetP - deviceSet_.activePowerSetting_41546) > kDeadband) {
        bus_->writeRegister(41546, static_cast<uint16_t>(targetP), "pcsset");
        LOG_ACTION("PCS有功功率指令下发:" + std::to_string(targetP / 10.0) + " kW");
    }
    if (std::abs(targetQ - deviceSet_.reactivePowerSetting_41547) > kDeadband) {
        bus_->writeRegister(41547, static_cast<uint16_t>(targetQ), "pcsset");
        LOG_ACTION("PCS无功功率指令下发:" + std::to_string(targetQ / 10.0) + " kvar");
    }
}

void PCS_Smarten::pcsSetData(MySQLDatabase& db)
{
    struct SetMap {
        int regAddr;
        int dbAddr;
        int readOffset;
        int scale;
        uint16_t SmartenSetData::* field;
    };
    static const SetMap kSetMaps[] = {
        {41473, 1, 0, 10, &SmartenSetData::dcVoltageLowerLimit_41473},
        {41474, 2, 1, 10, &SmartenSetData::constantVoltageChargeVoltage_41474},
        {41475, 3, 2, 10, &SmartenSetData::dcOutputVoltage_41475},
        {41478, 4, 5, 10, &SmartenSetData::dischargeTerminationVoltage_41478},
        {41480, 5, 7, 10, &SmartenSetData::chargeCutoffCurrent_41480},
        {41489, 6, 16, 10, &SmartenSetData::batteryProtectionSoc_41489},
        {41490, 7, 17, 10, &SmartenSetData::dischargeLimitSoc_41490},
        {41491, 8, 18, 10, &SmartenSetData::chargeLimitSoc_41491},
        {41682, 9, -1, 1, &SmartenSetData::vsgeEnable_41682},
        {41687, 10, -1, 1, &SmartenSetData::vsgeControlMode_41687},
        {41580, 11, -1, 1, &SmartenSetData::acConnectType_41580},
        {41582, 12, -1, 1, &SmartenSetData::threePhaseUnbalancedMode_41582},
        {41543, 13, -1, 1, &SmartenSetData::ratedVoltageLevel_41543},
        {41544, 14, -1, 1, &SmartenSetData::ratedFrequencyLevel_41544},
        {41701, 18, -1, 1, &SmartenSetData::energyControlMode_41701},
    };
    constexpr int kReg41696 = 41696;

    pcsRead_ = db.select(19, "pcsset");
    pcsWrite_ = db.select(20, "pcsset");
    if (pcsRead_ != 1 && pcsWrite_ != 1) {
        return;
    }
    auto refresh = [this, kReg41696]() -> bool {
        if (bus_->readRegisters(41473, 19, arr_) == -1) {
            return false;
        }
        for (const auto& m : kSetMaps) {
            if (m.readOffset >= 0) {
                deviceSet_.*(m.field) = arr_[m.readOffset];
            }
        }
        for (const auto& m : kSetMaps) {
            if (m.readOffset < 0) {
                if (bus_->readRegisters(m.regAddr, 1, arr_) == -1) {
                    return false;
                }
                deviceSet_.*(m.field) = arr_[0];
            }
        }
        if (bus_->readRegisters(kReg41696, 1, arr_) == -1) {
            return false;
        }
        apply41696ControlBits(deviceSet_, arr_[0]);
        return true;
    };
    if (!refresh()) {
        return;
    }

    if (pcsRead_ == 1) {
        listPcs_.clearData();
        listPcs_.addData(1, deviceSet_.dcVoltageLowerLimit_41473 * 0.1);
        listPcs_.addData(2, deviceSet_.constantVoltageChargeVoltage_41474 * 0.1);
        listPcs_.addData(3, deviceSet_.dcOutputVoltage_41475 * 0.1);
        listPcs_.addData(4, deviceSet_.dischargeTerminationVoltage_41478 * 0.1);
        listPcs_.addData(5, deviceSet_.chargeCutoffCurrent_41480 * 0.1);
        listPcs_.addData(6, deviceSet_.batteryProtectionSoc_41489 * 0.1);
        listPcs_.addData(7, deviceSet_.dischargeLimitSoc_41490 * 0.1);
        listPcs_.addData(8, deviceSet_.chargeLimitSoc_41491 * 0.1);
        listPcs_.addData(9, deviceSet_.vsgeEnable_41682);
        listPcs_.addData(10, deviceSet_.vsgeControlMode_41687);
        listPcs_.addData(11, deviceSet_.acConnectType_41580);
        listPcs_.addData(12, deviceSet_.threePhaseUnbalancedMode_41582);
        listPcs_.addData(13, deviceSet_.ratedVoltageLevel_41543);
        listPcs_.addData(14, deviceSet_.ratedFrequencyLevel_41544);
        listPcs_.addData(15, deviceSet_.antiBackflowProtection_41696_bit1 ? 1.0 : 0.0);
        listPcs_.addData(16, deviceSet_.powerFactorControl_41696_bit2 ? 1.0 : 0.0);
        listPcs_.addData(17, deviceSet_.threePhaseUnbalancedMode_41696_bit3 ? 1.0 : 0.0);
        listPcs_.addData(18, deviceSet_.energyControlMode_41701);
        db.insert(listPcs_.spliceData("pcsset"));
        db.update(19, 0, "pcsset");
    }

    if (pcsWrite_ == 1) {
        for (const auto& m : kSetMaps) {
            const double hmiValue = db.select(m.dbAddr, "pcsset");
            if (m.scale == 1) {
                hmiSet_.*(m.field) = static_cast<uint16_t>(hmiValue);
            } else {
                hmiSet_.*(m.field) = static_cast<uint16_t>(std::lround(hmiValue * m.scale));
            }
        }
        hmiSet_.antiBackflowProtection_41696_bit1 = db.select(15, "pcsset") != 0;
        hmiSet_.powerFactorControl_41696_bit2 = db.select(16, "pcsset") != 0;
        hmiSet_.threePhaseUnbalancedMode_41696_bit3 = db.select(17, "pcsset") != 0;
        for (const auto& m : kSetMaps) {
            const uint16_t desired = hmiSet_.*(m.field);
            if ((deviceSet_.*(m.field)) != desired) {
                bus_->writeRegister(m.regAddr, desired, "pcsset");
            }
        }
        const uint16_t cur = pack41696ControlWord(deviceSet_.antiBackflowProtection_41696_bit1,
                                                  deviceSet_.powerFactorControl_41696_bit2,
                                                  deviceSet_.threePhaseUnbalancedMode_41696_bit3);
        const uint16_t des = pack41696ControlWord(hmiSet_.antiBackflowProtection_41696_bit1,
                                                  hmiSet_.powerFactorControl_41696_bit2,
                                                  hmiSet_.threePhaseUnbalancedMode_41696_bit3);
        if (cur != des) {
            bus_->writeRegister(kReg41696, des, "pcsset");
        }
        db.update(20, 0, "pcsset");
    }
}

void PCS_Smarten::onAfterTelemetry(ModbusPollEngine& eng, MySQLDatabase& db)
{
    fillVirtual(eng);
    writeLogicData(eng, db);
    dispatchHmiControlCommands(db);
    pcsSetData(db);
}

void PCS_Smarten::runPcsThread(MySQLConnectionPool& pool)
{
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 600;
    const auto period = std::chrono::milliseconds(pollMs);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            // pollOnce 成功解码后会调 postDecodeHook → onAfterTelemetry（控制/写参）
            engine_->pollOnce(db);
        } catch (const std::exception& e) {
            std::cerr << "[pcs_smarten] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[pcs_smarten] unknown error" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}
