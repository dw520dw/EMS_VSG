
// =====================================================================
// Bamu.cpp
// 电池管理主控单元（BAMU）实现文件
// 负责 BMS 电芯数据采集、空调通信告警、DI 信号读取及逻辑数据写入
// =====================================================================

#include "Bamu.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {

// 判断指定遥测值是否处于告警状态（> 0.5 视为真）
inline bool alarmOn(const ModbusPollEngine& eng, const char* name)
{
    return eng.getValue(name) > 0.5;
}

}  // namespace

// ========== 构造/析构函数 ==========

// 构造函数：加载配置、建立三个轮询引擎（堆栈/BMS/空调）及后解码钩子
Bamu::Bamu(const std::string& configPath)
{
    // 确定配置文件路径：优先使用传入参数，否则使用全局默认配置
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;
    auto load = [&](const char* id) { return loadModbusDeviceProfile(path, id); };

    // --- 1. 堆栈引擎（Stack） ---
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

    // --- 2. BMS 引擎 ---
    bmsEngine_ = std::make_unique<ModbusPollEngine>(*bus_, load(Config::BAMU_BMS_DEVICE_ID));
    bmsEngine_->setSkipProbe(true);
    bmsEngine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterBms(eng, static_cast<MySQLDatabase&>(sink));
    });

    // --- 3. 空调引擎（Air） ---
    airEngine_ = std::make_unique<ModbusPollEngine>(*bus_, load(Config::BAMU_AIR_DEVICE_ID));
    airEngine_->setSkipProbe(true);
    airEngine_->setInheritCommFlag(0);
    airEngine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterAir(eng, static_cast<MySQLDatabase&>(sink));
    });

    std::cout << "[bamu] loaded stack/bms/air engines" << std::endl;
}

// 析构函数：断开 Modbus TCP 连接，释放资源
Bamu::~Bamu()
{
    if (bus_) {
        bus_->disconnect();
    }
}

// ========== 数据库列表刷新 ==========

// 将数据库列表数据批量写入指定表
void Bamu::flushDbList(MySQLDatabase& db, const std::string& table)
{
    db.insert(dbList_.spliceData(table));
}

// ========== 告警等级计算 ==========

// 计算当前告警等级（0=无告警, 1=一级, 2=二级, 3=三级/故障）
int Bamu::computeAlarmLevel(const ModbusPollEngine& eng) const
{
    // 三级告警（故障级）：模块过压/欠压/过温/欠温、电芯过压/欠压/过温/欠温、
    // SOC 低/高、SOH 低、组过温/欠压/过压/过流、绝缘低、电芯压差/温差、
    // 从机/主机断开、组电压异常、接触器异常、采集线断开、故障
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
    // 二级告警：模块/电芯/SOC/SOH/组相关告警 + 充/放电停止
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
    // 一级告警：模块/电芯/SOC/SOH/组相关告警（最低级别）
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
    return 0;  // 无告警
}

// ========== 堆栈数据后处理 ==========

// 堆栈数据后处理回调：计算有功功率、告警等级，处理 VSG 停机指令
void Bamu::onAfterStack(ModbusPollEngine& eng, MySQLDatabase& db)
{
    const double v = eng.getValue("TotalVoltage");
    const double i = eng.getValue("TotalCurrent");
    // 有功功率 = 电压 × 电流 × 缩放系数（单位 kW）
    eng.setValue("ActivePower", v * i * BamuRatio_Value3);
    // 告警等级
    eng.setValue("AlarmLevel", static_cast<double>(computeAlarmLevel(eng)));

    // VSG：data_total.112=1 → 写 503=2 停机
    if (db.select(112, "data_total")) {
        bus_->writeRegister(503, 2, "bamu");
        LOG_ACTION("BAMU系统停止, 写入503寄存器, 值为2");
    }
}

// ========== BMS 数据后处理 ==========

// BMS 数据后处理回调：计算 BMS 总功率
void Bamu::onAfterBms(ModbusPollEngine& eng, MySQLDatabase& /*db*/)
{
    const double v = eng.getValue("TotalVoltage");
    const double i = eng.getValue("TotalCurrent");
    // 功率 = 电压 × 电流 × 缩放系数（单位 kW）
    eng.setValue("Power", v * i * BamuRatio_Value3);
}

// ========== 空调数据后处理 ==========

// 空调数据后处理回调：风扇/水泵/压缩机状态、在线状态、通信告警
void Bamu::onAfterAir(ModbusPollEngine& eng, MySQLDatabase& db)
{
    // 风扇状态：转速 > 5 视为运行
    eng.setValue("FanStatus", eng.getValue("_fan_speed") > 5.0 ? 1.0 : 0.0);
    // 水泵状态：转速 > 5 视为运行
    eng.setValue("WaterPumpStatus", eng.getValue("_pump_speed") > 5.0 ? 1.0 : 0.0);
    // 压缩机状态：转速 > 5 视为运行
    eng.setValue("Compressor", eng.getValue("_comp_speed") > 5.0 ? 1.0 : 0.0);

    const int id = eng.profile().table_suffix;
    if (isValidAirUnitId(id)) {
        // 记录空调单元在线状态
        airOnline_[static_cast<size_t>(id)] = eng.getValue("Online");
        // 更新通信告警表
        db.update(airUnitToComAlarmAddr(id),
                  static_cast<int>(eng.getValue("Online") > 0.5 ? 1 : 0), "com_alarm");
    }
}

// ========== DI 信号读取 ==========

// 读取 DI（数字量输入）信号
bool Bamu::readDiSignals(DiSignals& out)
{
    if (bus_->readInputBits(DI_START_ADDR, DI_COUNT, arr_input_bits_) != -1) {
        out.EPO = arr_input_bits_[1] != 0;           // 紧急停机信号
        out.TotalSwitch = arr_input_bits_[2] != 0;   // 总开关信号
        out.FireLevel1 = arr_input_bits_[4] != 0;    // 消防一级报警信号
        out.FireControl = arr_input_bits_[5] != 0;   // 消防控制信号
        return true;
    }
    return false;
}

// ========== 逻辑数据写入 ==========

// 将逻辑数据写入数据库 logic 表和 dodi 表
void Bamu::writeLogicData(MySQLDatabase& db, const ModbusPollEngine& stack)
{
    // --- 写入 logic 表 ---
    dbList_.clearData();
    const int clusterCount = clampBmsClusterCount(batteryNumber_);
    const int airUnitCount = bmsClusterToAirUnitId(clusterCount);
    // 各空调单元在线状态
    for (int airUnitId = 1; airUnitId <= airUnitCount; ++airUnitId) {
        dbList_.addData(airUnitToLogicCommAddr(airUnitId),
                        static_cast<int>(airOnline_[static_cast<size_t>(airUnitId)] > 0.5 ? 1 : 0));
    }
    dbList_.addData(401, stack.commFlag());                          // 通信状态标志
    dbList_.addData(402, stack.getValue("SOC"));                     // SOC
    dbList_.addData(403, stack.getValue("MaxChargePower"));          // 最大充电功率
    dbList_.addData(404, stack.getValue("MaxDischargePower"));       // 最大放电功率
    dbList_.addData(405, stack.getValue("AlarmLevel"));              // 告警等级
    dbList_.addData(406, static_cast<int>(diSignals_.EPO));          // 紧急停机
    dbList_.addData(411, stack.getValue("ActivePower"));             // 有功功率
    dbList_.addData(412, static_cast<int>(diSignals_.FireLevel1));   // 消防一级报警
    dbList_.addData(413, static_cast<int>(diSignals_.FireControl));  // 消防控制
    dbList_.addData(10, static_cast<int>(diSignals_.TotalSwitch));   // 总开关
    flushDbList(db, "logic");

    // --- 写入 dodi 表 ---
    dbList_.clearData();
    dbList_.addData(4, static_cast<int>(diSignals_.EPO));            // 紧急停机
    dbList_.addData(5, static_cast<int>(diSignals_.FireLevel1));     // 消防一级报警
    dbList_.addData(6, static_cast<int>(diSignals_.FireControl));    // 消防控制
    dbList_.addData(7, static_cast<int>(diSignals_.TotalSwitch));    // 总开关
    flushDbList(db, "dodi");
}

// ========== 电芯数据采集 ==========

// 读取指定簇的电芯数据（电压 + 温度）
void Bamu::readBmsCellData(int id)
{
    const int addressOffset = (id - 1) * 3000;  // 每个簇的寄存器地址偏移

    // 读取电芯电压（第1块：191 + offset，120 个寄存器）
    if (bus_->readInputRegisters(191 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i] = arr_input_registers_[i];
        }
    }
    // 读取电芯电压（第2块：311 + offset，120 个寄存器）
    if (bus_->readInputRegisters(311 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i + BMS_CELL_MODBUS_BLOCK] =
                arr_input_registers_[i];
        }
    }
    // 读取电芯电压（第3块：431 + offset，20 个寄存器）
    if (bus_->readInputRegisters(431 + addressOffset, BMS_CELL_MODBUS_BLOCK2, arr_input_registers_) !=
        -1) {
        const int base = BMS_CELL_MODBUS_BLOCK * 2;
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK2; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Voltage[base + i] = arr_input_registers_[i];
        }
    }
    // 读取电芯温度（第1块：891 + offset，120 个寄存器，值 - 40 为实际温度）
    if (bus_->readInputRegisters(891 + addressOffset, BMS_CELL_MODBUS_BLOCK, arr_input_registers_) !=
        -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Temperature[i] =
                static_cast<uint16_t>(arr_input_registers_[i] - 40);
        }
    }
    // 读取电芯温度（第2块：1011 + offset，15 个寄存器）
    if (bus_->readInputRegisters(1011 + addressOffset, BMS_CELL_MODBUS_BLOCK1,
                                 arr_input_registers_) != -1) {
        for (int i = 0; i < BMS_CELL_MODBUS_BLOCK1; ++i) {
            bmsCell_[static_cast<size_t>(id)].Cell_Temperature[BMS_CELL_MODBUS_BLOCK + i] =
                static_cast<uint16_t>(arr_input_registers_[i] - 40);
        }
    }
}

// ========== 电芯数据写入 ==========

// 将指定簇的电芯数据写入数据库（电压 + 温度，变化检测）
void Bamu::writeCellData(MySQLDatabase& db, int id)
{
    auto& lastVol = lastVolDb_[static_cast<size_t>(id)];
    auto& lastTemp = lastTempDb_[static_cast<size_t>(id)];
    bool& hasVol = hasVolSnap_[static_cast<size_t>(id)];
    bool& hasTemp = hasTempSnap_[static_cast<size_t>(id)];

    // 写入电芯电压（变化检测写入，仅值变化时写入数据库）
    dbList_.clearData();
    for (int i = 0; i < BMS_CELL_VOLTAGE_COUNT; ++i) {
        addDbDataIfChanged(dbList_, i + 1,
                           bmsCell_[static_cast<size_t>(id)].Cell_Voltage[i] * BamuRatio_Value3,
                           lastVol, hasVol);
    }
    hasVol = true;
    flushDbList(db, "bms_cellvol" + std::to_string(id));

    // 写入电芯温度（变化检测写入）
    dbList_.clearData();
    for (int i = 0; i < BMS_CELL_TEMPERATURE_COUNT; ++i) {
        addDbDataIfChanged(dbList_, i + 1,
                           static_cast<double>(bmsCell_[static_cast<size_t>(id)].Cell_Temperature[i]),
                           lastTemp, hasTemp);
    }
    hasTemp = true;
    flushDbList(db, "bms_celltemp" + std::to_string(id));
}

// ========== 堆栈数据处理 ==========

// 处理堆栈数据：轮询计数控制、读取 DI 信号、写入逻辑数据
void Bamu::processStack(MySQLDatabase& db)
{
    // 按配置间隔读取电池数量（qt 表）
    if (++qtPollCounter_ >= Config::BAMU_QT_CONFIG_POLL_INTERVAL || batteryNumber_ < 1) {
        qtPollCounter_ = 0;
        batteryNumber_ = db.select(stackEngine_->profile().count_qt_addr, "qt");
    }
    stackEngine_->pollOnce(db);
    readDiSignals(diSignals_);
    writeLogicData(db, *stackEngine_);
}

// ========== BMS 簇数据处理 ==========

// 处理指定簇的 BMS 数据（含电芯电压/温度采集）
void Bamu::processCluster(MySQLDatabase& db, int id)
{
    // 如果堆栈通信异常，跳过本簇处理
    if (stackEngine_->commFlag() != 0) {
        bmsEngine_->setTableSuffix(id);
        bmsEngine_->setInheritCommFlag(1);
        bmsEngine_->setSkipProbe(true);
        bmsEngine_->pollOnce(db);
        return;
    }

    // 正常流程：轮询 BMS 数据
    bmsEngine_->setTableSuffix(id);
    bmsEngine_->setClusterIndex(id);
    bmsEngine_->setSkipProbe(true);
    bmsEngine_->setInheritCommFlag(0);
    bmsEngine_->pollOnce(db);

    // 如果是空调单元的主簇，同时轮询对应空调数据
    if (isAirUnitLeadCluster(id)) {
        const int airUnitId = bmsClusterToAirUnitId(id);
        airEngine_->setTableSuffix(airUnitId);
        airEngine_->setClusterIndex(airUnitId);
        airEngine_->setSkipProbe(true);
        airEngine_->setInheritCommFlag(0);
        airEngine_->pollOnce(db);
    }

    // 按配置间隔读取电芯数据（电压 + 温度）
    const auto now = std::chrono::steady_clock::now();
    if (lastCellRealtime_[static_cast<size_t>(id)] == std::chrono::steady_clock::time_point{} ||
        now - lastCellRealtime_[static_cast<size_t>(id)] >=
            std::chrono::milliseconds(Config::BMS_CELL_REALTIME_INTERVAL)) {
        readBmsCellData(id);
        writeCellData(db, id);
        lastCellRealtime_[static_cast<size_t>(id)] = now;
    }
}

// ========== 主循环线程 ==========

// BAMU 主循环线程：以固定周期轮询堆栈、BMS、空调数据
void Bamu::runBamuDataThread(MySQLConnectionPool& pool)
{
    // 启动前等待 1 秒，确保其他模块初始化完成
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto period = std::chrono::milliseconds(Config::BAMU_DATA_INTERVAL);
    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            // 1. 处理堆栈数据（含 DI 信号读取、逻辑数据写入）
            processStack(db);
            // 2. 依次处理每个 BMS 簇（含空调轮询、电芯数据采集）
            const int n = clampBmsClusterCount(batteryNumber_);
            for (int i = 1; i <= n; ++i) {
                processCluster(db, i);
            }
        } catch (const std::exception& e) {
            std::cerr << "[bamu] " << e.what() << std::endl;
        }
        // 自适应休眠：扣除本轮耗时，保证固定周期
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}