
/**
 * @file Dido.cpp
 * @brief Dido（数字量输入输出）Modbus TCP 通信驱动实现
 *
 * 本模块负责通过 Modbus TCP 协议与 Dido 设备进行通信，主要功能包括：
 * 1. 周期性轮询设备遥测数据（开关状态、指示灯状态等）；
 * 2. 将 HMI 控制指令下发至设备（黄灯/绿灯/红灯/柴发开关/总开关）；
 * 3. 读取设备反馈的开关状态并写入数据库；
 * 4. 计算虚拟量（指示灯状态、开关 DI 状态等）。
 */

#include "Dido.h"
#include "Config.h"
#include "ModbusConfigLoader.h"
#include "logger.h"
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

// ========== 构造/析构函数 ==========

Dido::Dido(const std::string& configPath)
{
    // 确定配置文件路径：优先使用传入参数，否则使用全局默认配置
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;

    // 从配置文件加载 Dido 设备的 Modbus 通信参数
    ModbusDeviceProfile p = loadModbusDeviceProfile(path, Config::DIDO_DEVICE_ID);
    if (p.tcp_ip.empty()) {
        throw std::runtime_error("dido: tcp_ip empty in " + path);
    }

    // 创建 Modbus TCP 通信总线对象
    bus_ = std::make_unique<ModbusTCP>(p.tcp_ip, p.tcp_port, p.slave, p.timeout_ms);

    // 创建轮询引擎并绑定通信总线和设备配置
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(p));

    // 注册后解码钩子：每次成功解码遥测数据后自动调用 onAfterDecode
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterDecode(eng, static_cast<MySQLDatabase&>(sink));
    });

    // 打印连接信息便于调试
    std::cout << "[dido] tcp=" << engine_->profile().tcp_ip << ":" << engine_->profile().tcp_port
              << std::endl;
}

Dido::~Dido()
{
    // 断开 Modbus TCP 连接，释放资源
    if (bus_) {
        bus_->disconnect();
    }
}

// ========== writeDoIfChanged ==========

void Dido::writeDoIfChanged(int reg, bool desired, bool& actual, const char* name)
{
    // 目标值与当前值相同则跳过，避免无效写入
    if (desired == actual) {
        return;
    }
    const uint16_t val = desired ? 1 : 0;
    if (bus_->writeRegister(reg, val, engine_->profile().resolvedMysqlTable()) == -1) {
        return;
    }
    actual = desired;
    // 记录操作日志：寄存器地址 + 使能/关闭 + 名称
    LOG_ACTION(std::to_string(reg) + (val ? "使能" : "关闭") + name);
}

// ========== onAfterDecode ==========

void Dido::onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db)
{
    // --- 1. 读取 HMI 控制指令（从 data_total 表读取目标值）---
    greenLed_ = db.select(101, "data_total") != 0;      // 绿灯目标值
    yellowLed_ = db.select(102, "data_total") != 0;     // 黄灯目标值
    redLed_ = db.select(103, "data_total") != 0;        // 红灯目标值
    chiFaSwitch_ = db.select(107, "data_total") != 0;   // 柴发开关目标值
    qacSplit_ = db.select(116, "data_total") != 0;      // 总开关目标值

    // --- 2. 读取设备反馈的当前状态（寄存器 200~204）---
    if (bus_->readRegisters(200, 5, arr_) != -1) {
        readYellow_ = arr_[0] != 0;   // 黄灯当前状态
        readGreen_ = arr_[1] != 0;    // 绿灯当前状态
        readRed_ = arr_[2] != 0;      // 红灯当前状态
        readChiFa_ = arr_[3] != 0;    // 柴发开关当前状态
        readQac_ = arr_[4] != 0;      // 总开关当前状态

        // --- 3. 比较 HMI 目标值与设备当前值，不同则下发 DO 指令 ---
        writeDoIfChanged(200, yellowLed_, readYellow_, "黄灯");
        writeDoIfChanged(201, greenLed_, readGreen_, "绿灯");
        writeDoIfChanged(202, redLed_, readRed_, "红灯");
        writeDoIfChanged(203, chiFaSwitch_, readChiFa_, "柴发开关");
        writeDoIfChanged(204, qacSplit_, readQac_, "总开关");
    }

    // --- 4. 写入虚拟量到轮询引擎 ---
    eng.setValue("YellowLight", yellowLed_ ? 1.0 : 0.0);   // 黄灯状态
    eng.setValue("GreenLight", greenLed_ ? 1.0 : 0.0);     // 绿灯状态
    eng.setValue("RedLight", redLed_ ? 1.0 : 0.0);         // 红灯状态
    eng.setValue("ChiFa_Switch_DO", chiFaSwitch_ ? 1.0 : 0.0); // 柴发开关 DO
    eng.setValue("QacSplit", qacSplit_ ? 1.0 : 0.0);       // 总开关状态

    // --- 5. 读取设备 DI 状态并写入数据库 logic 表 ---
    const int chiFa = eng.getValue("ChiFa_Switch_DI") > 0.5 ? 1 : 0;  // 柴发开关 DI
    const int pv = eng.getValue("PV_Switch_DI") > 0.5 ? 1 : 0;        // PV 开关 DI
    const int load = eng.getValue("Load_Switch_DI") > 0.5 ? 1 : 0;    // 负载开关 DI
    const int sign = eng.getValue("ChiFa_Singn_DI") > 0.5 ? 1 : 0;    // 柴发信号 DI
    db.update(6, chiFa, "logic");      // 柴发开关状态
    db.update(9, pv, "logic");         // PV 开关状态
    db.update(7, load, "logic");       // 负载开关状态
    db.update(451, sign, "logic");     // 柴发信号状态
}

// ========== didoThread ==========

void Dido::didoThread(MySQLConnectionPool& pool)
{
    // 启动前等待 1 秒，确保其他模块初始化完成
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 获取轮询周期，未配置时默认 500ms
    const int pollMs = engine_->profile().poll_ms > 0 ? engine_->profile().poll_ms : 500;
    const auto period = std::chrono::milliseconds(pollMs);

    while (true) {
        const auto t0 = std::chrono::steady_clock::now();
        try {
            MySQLDatabase db(pool);
            // pollOnce 成功解码后会调 postDecodeHook → onAfterDecode
            engine_->pollOnce(db);
        } catch (const std::exception& e) {
            std::cerr << "[dido] " << e.what() << std::endl;
        } catch (...) {
            std::cerr << "[dido] unknown error" << std::endl;
        }

        // 自适应休眠：扣除本轮耗时，保证固定周期
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}