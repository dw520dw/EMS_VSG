
// PCS_Smarten 储能变流器 Modbus TCP 通信驱动模块
// 功能：通过 Modbus TCP 协议与 Smarten PCS 设备进行通信，采集遥测数据，
//       下发 HMI 控制指令，处理 PCS 参数读写。

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

// ========== 位操作辅助函数 ==========

// 从 16 位字中提取指定位的值，赋给布尔标志
inline void setBitFlag(bool& flag, uint16_t word, int bit)
{
    flag = (word & (1U << bit)) != 0;
}

// 解析寄存器 41696 的控制位，填充到 SmartenSetData 结构体
// bit1=防逆流保护, bit2=功率因数控制, bit3=三相不平衡模式
inline void apply41696ControlBits(SmartenSetData& dst, uint16_t word)
{
    setBitFlag(dst.antiBackflowProtection_41696_bit1, word, 1);
    setBitFlag(dst.powerFactorControl_41696_bit2, word, 2);
    setBitFlag(dst.threePhaseUnbalancedMode_41696_bit3, word, 3);
}

// 将三个布尔控制标志打包为寄存器 41696 的 16 位控制字
// bit0 固定为 0（保留位）
inline uint16_t pack41696ControlWord(bool antiBackflow, bool powerFactor, bool unbalanced)
{
    uint16_t v = 0x0000;
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

// ========== SunPv 类实现 ==========

// 构造函数：加载配置、建立 TCP 连接、注册后解码钩子
// 参数 configPath: 配置文件路径，为空时使用 Config 默认路径
// 抛出 std::runtime_error：当配置中 tcp_ip 为空时
PCS_Smarten::PCS_Smarten(const std::string& configPath)
{
    // 确定配置文件路径：优先使用传入参数，否则使用全局默认配置
    const std::string path =
        configPath.empty() ? std::string(Config::MODBUS_DEVICES_CONFIG) : configPath;

    // 从配置文件加载 PCS Smarten 设备的 Modbus 通信参数
    ModbusDeviceProfile profile = loadModbusDeviceProfile(path, Config::PCS_SMARTEN_DEVICE_ID);
    if (profile.tcp_ip.empty()) {
        throw std::runtime_error("pcs_smarten: tcp_ip empty in " + path);
    }

    // 创建 Modbus TCP 通信总线对象
    bus_ = std::make_unique<ModbusTCP>(profile.tcp_ip, profile.tcp_port, profile.slave,
                                       profile.timeout_ms);

    // 创建轮询引擎并绑定通信总线和设备配置
    engine_ = std::make_unique<ModbusPollEngine>(*bus_, std::move(profile));

    // 注册后解码钩子：每次成功解码遥测数据后自动调用 onAfterTelemetry
    engine_->setPostDecodeHook([this](ModbusPollEngine& eng, IDataSink& sink) {
        onAfterTelemetry(eng, static_cast<MySQLDatabase&>(sink));
    });

    // 打印连接信息便于调试
    std::cout << "[pcs_smarten] tcp=" << engine_->profile().tcp_ip << ":"
              << engine_->profile().tcp_port << std::endl;
}

// 析构函数：断开 Modbus TCP 连接，释放资源
PCS_Smarten::~PCS_Smarten()
{
    if (bus_) {
        bus_->disconnect();
    }
}

// 读取 4 个连续寄存器拼接为 64 位无符号整数，并按比例缩放
// 参数 startAddr: 起始寄存器地址（大端序，w[0]为最高位）
// 参数 scale: 缩放系数
// 返回: 缩放后的浮点数值；读取失败返回 0.0
double PCS_Smarten::readU64Scaled(int startAddr, double scale)
{
    uint16_t w[4] = {};
    if (bus_->readRegisters(startAddr, 4, w) == -1) {
        return 0.0;
    }
    // 大端序拼接：w[0]<<48 | w[1]<<32 | w[2]<<16 | w[3]
    const uint64_t raw = (static_cast<uint64_t>(w[0]) << 48) | (static_cast<uint64_t>(w[1]) << 32) |
                         (static_cast<uint64_t>(w[2]) << 16) | static_cast<uint64_t>(w[3]);
    return static_cast<double>(raw) * scale;
}

// 根据底层寄存器状态计算并填充虚拟量到轮询引擎
// 参数 eng: 轮询引擎引用，用于读取原始值和写入虚拟量
// 计算的虚拟量：
//   Status: 综合运行状态（0=停机, 1=运行, 2=告警, 3=故障）
//   OnGrid: 并离网状态（0=并网, 1=离网, 2=未知）
//   DCDischargeEnergy / DCChargeEnergy: 直流侧累计放电/充电量（kWh）
//   ACDischargeEnergy / ACChargeEnergy: 交流侧累计放电/充电量（kWh）
void PCS_Smarten::fillVirtual(ModbusPollEngine& eng)
{
    // --- 1. 计算综合运行状态 Status ---
    const bool fault = eng.getValue("FaultState") > 0.5;
    const bool alarm = eng.getValue("AlarmState") > 0.5;

    // 停机类状态位：任一为真则视为停机态
    const bool isStopLike = eng.getValue("_st41726_b1") > 0.5 || eng.getValue("_st41726_b2") > 0.5 ||
                            eng.getValue("_st41726_b3") > 0.5 || eng.getValue("_st41726_b9") > 0.5 ||
                            eng.getValue("_st41726_b10") > 0.5 || eng.getValue("EmergencyShutdown") > 0.5;

    // 运行类状态位：任一为真则视为运行态
    const bool isRunningLike = eng.getValue("_st41726_b6") > 0.5 || eng.getValue("_st41726_b7") > 0.5 ||
                               eng.getValue("_st41726_b8") > 0.5 || eng.getValue("_st41726_b11") > 0.5 ||
                               eng.getValue("_st41726_b12") > 0.5;

    int status = 0;  // 默认：停机
    if (fault) {
        status = 3;  // 故障优先级最高
    } else if (alarm) {
        status = 2;  // 告警次之
    } else if (isRunningLike) {
        status = 1;  // 运行
    } else if (isStopLike) {
        status = 0;  // 停机
    }
    eng.setValue("Status", static_cast<double>(status));

    // --- 2. 计算并离网状态 OnGrid ---
    const bool onGrid = eng.getValue("_st41726_b4") > 0.5;
    const bool offGrid = eng.getValue("_st41726_b5") > 0.5;
    int grid = 2;  // 默认：未知
    if (onGrid) {
        grid = 0;  // 并网
    } else if (offGrid) {
        grid = 1;  // 离网
    }
    eng.setValue("OnGrid", static_cast<double>(grid));

    // --- 3. 读取累计充放电量（64位寄存器，缩放系数0.1 → kWh）---
    eng.setValue("DCDischargeEnergy", readU64Scaled(41845, 0.1));
    eng.setValue("DCChargeEnergy", readU64Scaled(41849, 0.1));
    eng.setValue("ACDischargeEnergy", readU64Scaled(40173, 0.1));
    eng.setValue("ACChargeEnergy", readU64Scaled(40177, 0.1));
}

// 将关键逻辑数据写入 MySQL 数据库 logic 表
// 参数 eng: 轮询引擎引用（获取通信标志和虚拟量）
// 参数 db:  数据库操作对象
void PCS_Smarten::writeLogicData(ModbusPollEngine& eng, MySQLDatabase& db)
{
    db.update(101, eng.commFlag(), "logic");                          // 通信状态标志
    db.update(102, eng.getValue("Status"), "logic");                  // 综合运行状态
    db.update(103, eng.getValue("ACActivePower"), "logic");           // 交流有功功率
    db.update(105, eng.getValue("OnGrid"), "logic");                  // 并离网状态
    db.update(106, eng.getValue("_st41727_b4") > 0.5 ? 1.0 : 0.0, "logic"); // 备用状态位
}

// 读取设备当前设定值与 HMI 目标值，比较后下发控制指令
// 参数 db: 数据库操作对象（读取 HMI 指令、更新复位标志）
// 支持的控制指令：
//   PCS 开机/关机（寄存器 41379）
//   并网/离网切换（寄存器 41671）
//   告警复位（寄存器 41378）
//   有功功率设定（寄存器 41546，带 ±1 死区，单位 0.1kW）
//   无功功率设定（寄存器 41547，带 ±1 死区，单位 0.1kvar）
void PCS_Smarten::dispatchHmiControlCommands(MySQLDatabase& db)
{
    // === 1. 读取设备当前设定值 ===
    if (bus_->readRegisters(41378, 2, arr_) != -1) {
        deviceSet_.alarmReset_41378 = arr_[0];       // 告警复位寄存器当前值
        deviceSet_.pcsOnOff_41379 = arr_[1];         // 开关机寄存器当前值
    }
    if (bus_->readRegisters(41546, 2, arr_) != -1) {
        deviceSet_.activePowerSetting_41546 = static_cast<int16_t>(arr_[0]);   // 有功功率当前值
        deviceSet_.reactivePowerSetting_41547 = static_cast<int16_t>(arr_[1]); // 无功功率当前值
    }
    if (bus_->readRegisters(41671, 1, arr_) != -1) {
        deviceSet_.gridInterMode_41671 = arr_[0];    // 并离网模式当前值
    }

    // === 2. 读取 HMI 目标设定值 ===
    hmiSet_.alarmReset_41378 = static_cast<uint16_t>(db.select(602, "qt"));

    // 批量读取 HMI 控制指令（地址 108/109/113/115）
    static const std::vector<int> kAddrs{108, 109, 113, 115};
    const auto hmiCmd = db.selectMultipleData("data_total", kAddrs);
    hmiSet_.pcsOnOff_41379 = static_cast<uint16_t>(hmiCmd.at(108));      // 开关机指令
    hmiSet_.gridInterMode_41671 = static_cast<uint16_t>(hmiCmd.at(115)); // 并离网指令

    // 有功/无功功率取反并放大10倍（HMI单位kW/kvar → 设备单位0.1kW/0.1kvar）
    const int targetP = - static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(109)) * 10));
    const int targetQ = - static_cast<int>(std::lround(static_cast<double>(hmiCmd.at(113)) * 10));
    hmiSet_.activePowerSetting_41546 = static_cast<int16_t>(targetP);
    hmiSet_.reactivePowerSetting_41547 = static_cast<int16_t>(targetQ);

    // === 3. 比较并下发控制指令 ===

    // 3.1 开关机控制
    if (hmiSet_.pcsOnOff_41379 == 1 && deviceSet_.pcsOnOff_41379 != 1) {
        LOG_ACTION("PCS开机");
        bus_->writeRegister(41379, 1, "pcsset");
    } else if (hmiSet_.pcsOnOff_41379 == 0 && deviceSet_.pcsOnOff_41379 != 0) {
        LOG_ACTION("PCS关机");
        bus_->writeRegister(41379, 0, "pcsset");
    }

    // 3.2 并离网切换
    if (hmiSet_.gridInterMode_41671 == 0 && deviceSet_.gridInterMode_41671 != 0) {
        LOG_ACTION("PCS并网");
        bus_->writeRegister(41671, 0, "pcsset");
    } else if (hmiSet_.gridInterMode_41671 != 0 &&
               deviceSet_.gridInterMode_41671 != hmiSet_.gridInterMode_41671) {
        LOG_ACTION("PCS离网");
        bus_->writeRegister(41671, 1, "pcsset");
    }

    // 3.3 告警复位（写 0 触发复位，同时清除 HMI 复位标志）
    if (hmiSet_.alarmReset_41378 == 1) {
        bus_->writeRegister(41378, 0, "pcsset");
        LOG_ACTION("PCS告警复位");
        db.update(602, 0, "qt");
    }

    // 3.4 有功/无功功率下发（带死区判断，避免频繁写入）
    constexpr int kDeadband = 10; // 死区阈值：10 × 0.1 = 1 kW/kvar
    if (std::abs(targetP - deviceSet_.activePowerSetting_41546) > kDeadband) {
        bus_->writeRegister(41546, static_cast<uint16_t>(targetP), "pcsset");
        LOG_ACTION("PCS有功功率指令下发:" + std::to_string(targetP / 10.0) + " kW");
    }
    if (std::abs(targetQ - deviceSet_.reactivePowerSetting_41547) > kDeadband) {
        bus_->writeRegister(41547, static_cast<uint16_t>(targetQ), "pcsset");
        LOG_ACTION("PCS无功功率指令下发:" + std::to_string(targetQ / 10.0) + " kvar");
    }
}

// 处理 PCS 参数的批量读取与写入（与数据库 pcsset 表交互）
// 参数 db: 数据库操作对象
// 工作流程：
//   当 pcsRead_=1 时：从设备读取全部参数 → 写入数据库 → 清除读标志
//   当 pcsWrite_=1 时：从数据库读取目标值 → 与设备当前值比较 → 差异项写入设备 → 清除写标志
// 支持的参数共 18 项，涵盖电压限值、SOC 限值、VSGE 设置、额定参数、能量控制模式等。
// 其中寄存器 41696 为位域控制字，需特殊处理。
void PCS_Smarten::pcsSetData(MySQLDatabase& db)
{
    // 参数映射表条目：定义寄存器地址、数据库地址、批量读取偏移、缩放系数、结构体字段指针
    // readOffset >= 0 表示可通过批量读取（41473起19个寄存器）获取；
    // readOffset < 0 表示需要单独读取对应寄存器
    struct SetMap {
        int regAddr;                      // Modbus 寄存器地址
        int dbAddr;                       // 数据库 pcsset 表地址编号
        int readOffset;                   // 批量读取数组中的偏移，-1 表示需单独读取
        int scale;                        // 缩放系数（写入时 HMI值×scale → 寄存器值）
        uint16_t SmartenSetData::* field; // SmartenSetData 结构体成员指针
    };

    // 参数映射表：15 项常规参数
    static const SetMap kSetMaps[] = {
        {41473, 1, 0, 10, &SmartenSetData::dcVoltageLowerLimit_41473},         // 直流电压下限
        {41474, 2, 1, 10, &SmartenSetData::constantVoltageChargeVoltage_41474}, // 恒压充电电压
        {41475, 3, 2, 10, &SmartenSetData::dcOutputVoltage_41475},             // 直流输出电压
        {41478, 4, 5, 10, &SmartenSetData::dischargeTerminationVoltage_41478}, // 放电终止电压
        {41480, 5, 7, 10, &SmartenSetData::chargeCutoffCurrent_41480},         // 充电截止电流
        {41489, 6, 16, 10, &SmartenSetData::batteryProtectionSoc_41489},      // 电池保护 SOC
        {41490, 7, 17, 10, &SmartenSetData::dischargeLimitSoc_41490},          // 放电限制 SOC
        {41491, 8, 18, 10, &SmartenSetData::chargeLimitSoc_41491},            // 充电限制 SOC
        {41682, 9, -1, 1, &SmartenSetData::vsgeEnable_41682},                 // VSGE 使能
        {41687, 10, -1, 1, &SmartenSetData::vsgeControlMode_41687},           // VSGE 控制模式
        {41580, 11, -1, 1, &SmartenSetData::acConnectType_41580},             // 交流接入类型
        {41582, 12, -1, 1, &SmartenSetData::threePhaseUnbalancedMode_41582},  // 三相不平衡模式
        {41543, 13, -1, 1, &SmartenSetData::ratedVoltageLevel_41543},         // 额定电压等级
        {41544, 14, -1, 1, &SmartenSetData::ratedFrequencyLevel_41544},       // 额定频率等级
        {41701, 18, -1, 1, &SmartenSetData::energyControlMode_41701},         // 能量控制模式
    };
    constexpr int kReg41696 = 41696; // 控制字寄存器地址（位域，需特殊处理）

    // 检查是否有读/写请求
    pcsRead_ = db.select(19, "pcsset");
    pcsWrite_ = db.select(20, "pcsset");
    if (pcsRead_ != 1 && pcsWrite_ != 1) {
        return; // 无读写请求，直接返回
    }

    // Lambda：从设备刷新全部参数到 deviceSet_
    auto refresh = [this, kReg41696]() -> bool {
        // 批量读取 41473 起的 19 个寄存器
        if (bus_->readRegisters(41473, 19, arr_) == -1) {
            return false;
        }
        // 填充可批量读取的参数
        for (const auto& m : kSetMaps) {
            if (m.readOffset >= 0) {
                deviceSet_.*(m.field) = arr_[m.readOffset];
            }
        }
        // 单独读取不可批量获取的参数
        for (const auto& m : kSetMaps) {
            if (m.readOffset < 0) {
                if (bus_->readRegisters(m.regAddr, 1, arr_) == -1) {
                    return false;
                }
                deviceSet_.*(m.field) = arr_[0];
            }
        }
        // 读取并解析 41696 控制字
        if (bus_->readRegisters(kReg41696, 1, arr_) == -1) {
            return false;
        }
        apply41696ControlBits(deviceSet_, arr_[0]);
        return true;
    };

    // 执行刷新，失败则放弃本次读写
    if (!refresh()) {
        return;
    }

    // === 读请求：将设备参数写入数据库 ===
    if (pcsRead_ == 1) {
        listPcs_.clearData();
        // 带缩放的参数（÷10 还原为物理值）
        listPcs_.addData(1, deviceSet_.dcVoltageLowerLimit_41473 * 0.1);
        listPcs_.addData(2, deviceSet_.constantVoltageChargeVoltage_41474 * 0.1);
        listPcs_.addData(3, deviceSet_.dcOutputVoltage_41475 * 0.1);
        listPcs_.addData(4, deviceSet_.dischargeTerminationVoltage_41478 * 0.1);
        listPcs_.addData(5, deviceSet_.chargeCutoffCurrent_41480 * 0.1);
        listPcs_.addData(6, deviceSet_.batteryProtectionSoc_41489 * 0.1);
        listPcs_.addData(7, deviceSet_.dischargeLimitSoc_41490 * 0.1);
        listPcs_.addData(8, deviceSet_.chargeLimitSoc_41491 * 0.1);
        // 无缩放参数
        listPcs_.addData(9, deviceSet_.vsgeEnable_41682);
        listPcs_.addData(10, deviceSet_.vsgeControlMode_41687);
        listPcs_.addData(11, deviceSet_.acConnectType_41580);
        listPcs_.addData(12, deviceSet_.threePhaseUnbalancedMode_41582);
        listPcs_.addData(13, deviceSet_.ratedVoltageLevel_41543);
        listPcs_.addData(14, deviceSet_.ratedFrequencyLevel_41544);
        // 41696 控制字各位
        listPcs_.addData(15, deviceSet_.antiBackflowProtection_41696_bit1 ? 1.0 : 0.0);
        listPcs_.addData(16, deviceSet_.powerFactorControl_41696_bit2 ? 1.0 : 0.0);
        listPcs_.addData(17, deviceSet_.threePhaseUnbalancedMode_41696_bit3 ? 1.0 : 0.0);
        listPcs_.addData(18, deviceSet_.energyControlMode_41701);

        // 批量写入数据库并清除读请求标志
        db.insert(listPcs_.spliceData("pcsset"));
        db.update(19, 0, "pcsset");
    }

    // === 写请求：将数据库目标值下发到设备 ===
    if (pcsWrite_ == 1) {
        // 从数据库读取各参数目标值并缩放
        for (const auto& m : kSetMaps) {
            const double hmiValue = db.select(m.dbAddr, "pcsset");
            if (m.scale == 1) {
                hmiSet_.*(m.field) = static_cast<uint16_t>(hmiValue);
            } else {
                hmiSet_.*(m.field) = static_cast<uint16_t>(std::lround(hmiValue * m.scale));
            }
        }
        // 读取 41696 控制字各位的目标值
        hmiSet_.antiBackflowProtection_41696_bit1 = db.select(15, "pcsset") != 0;
        hmiSet_.powerFactorControl_41696_bit2 = db.select(16, "pcsset") != 0;
        hmiSet_.threePhaseUnbalancedMode_41696_bit3 = db.select(17, "pcsset") != 0;

        // 逐项比较，仅当目标值与设备当前值不同时才写入
        for (const auto& m : kSetMaps) {
            const uint16_t desired = hmiSet_.*(m.field);
            if ((deviceSet_.*(m.field)) != desired) {
                bus_->writeRegister(m.regAddr, desired, "pcsset");
            }
        }

        // 41696 控制字整体比较，不同则写入
        const uint16_t cur = pack41696ControlWord(deviceSet_.antiBackflowProtection_41696_bit1,
                                                  deviceSet_.powerFactorControl_41696_bit2,
                                                  deviceSet_.threePhaseUnbalancedMode_41696_bit3);
        const uint16_t des = pack41696ControlWord(hmiSet_.antiBackflowProtection_41696_bit1,
                                                  hmiSet_.powerFactorControl_41696_bit2,
                                                  hmiSet_.threePhaseUnbalancedMode_41696_bit3);
        if (cur != des) {
            bus_->writeRegister(kReg41696, des, "pcsset");
        }

        // 清除写请求标志
        db.update(20, 0, "pcsset");
    }
}

// 遥测后处理总入口：在每次成功解码遥测数据后被回调
// 参数 eng: 轮询引擎引用
// 参数 db:  数据库操作对象
// 依次执行：填充虚拟量 → 写入逻辑数据 → 下发 HMI 控制指令 → 处理参数读写
void PCS_Smarten::onAfterTelemetry(ModbusPollEngine& eng, MySQLDatabase& db)
{
    fillVirtual(eng);                // 步骤1：计算虚拟量
    writeLogicData(eng, db);         // 步骤2：写入逻辑数据到数据库
    dispatchHmiControlCommands(db);  // 步骤3：下发 HMI 控制指令
    pcsSetData(db);                  // 步骤4：处理参数读写请求
}

// PCS 通信主循环线程：以固定周期轮询设备数据
// 参数 pool: MySQL 连接池引用
// 循环流程：
//   1. 调用 engine_->pollOnce() 执行一次完整的 Modbus 轮询+解码；
//   2. 解码成功后自动触发 postDecodeHook → onAfterTelemetry；
//   3. 异常捕获并打印错误信息，不中断循环；
//   4. 自适应休眠，保证轮询周期稳定（默认 600ms）。
void PCS_Smarten::runPcsThread(MySQLConnectionPool& pool)
{
    // 启动前等待 1 秒，确保其他模块初始化完成
    std::this_thread::sleep_for(std::chrono::seconds(1));

    // 获取轮询周期，未配置时默认 600ms
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

        // 自适应休眠：扣除本轮耗时，保证固定周期
        const auto elapsed = std::chrono::steady_clock::now() - t0;
        if (elapsed < period) {
            std::this_thread::sleep_for(period - elapsed);
        }
    }
}