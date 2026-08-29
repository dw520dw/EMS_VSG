#ifndef MODBUSRTU_H
#define MODBUSRTU_H

#include "IModbusBus.h"
#include <modbus.h>
#include <chrono>
#include <iostream>
#include <mutex>
#include <string>

/**
 * Modbus RTU 传输（串口）。
 * 实现 IModbusBus，供电表等 RTU 设备与 ModbusPollEngine 使用。
 * 底层为 libmodbus：8N1；连接按需建立，串口失效（USB 转串口重枚举等）读写前自动重开。
 * 线程安全：每个公共方法由内置 io_mutex_ 串行化，同一实例可被多线程安全共享；
 * 但两个实例打开同一串口仍会在物理线上冲突，一端口一台设备/一组设备共用实例。
 */
class ModbusRTU : public IModbusBus {
public:
    /**
     * @param device    串口设备路径，如 /dev/ttyS1
     * @param baudrate  波特率，如 9600
     * @param timeout_ms 应答超时（毫秒）；0 表示默认 1000ms
     */
    ModbusRTU(const char* device, int baudrate, int timeout_ms = 0);
    ~ModbusRTU() override;

    /** 设置从站地址 */
    bool setSlave(int slave_id) override;

    bool isConnected() const override;
    void disconnect();

    int readRegisters(int addr, int num_reg, uint16_t* dest) override;       // FC3 保持寄存器
    int writeRegister(int addr, uint16_t value, const std::string& tableName) override;
    int writeCoil(int addr, bool value, const std::string& tableName) override;
    int writeRegisters(int addr, const uint16_t* values, int count, const std::string& tableName) override;  // FC16
    int writeBits(int addr, const uint8_t* values, int count, const std::string& tableName) override;        // FC15
    int readInputRegisters(int addr, int num_reg, uint16_t* dest) override;  // FC4
    int readBits(int addr, int num_reg, uint8_t* dest) override;             // FC1 线圈
    int readInputBits(int addr, int num_reg, uint8_t* dest) override;        // FC2 离散输入
    bool probe(int addr, int num_reg, uint16_t* dest, const std::string& tag) override;

private:
    /** 串口已打开则复用，否则重新 openPort()；读写前调用 */
    bool ensureConnected();
    /** 新建 ctx 并 connect 串口；失败释放 ctx 返回 false */
    bool openPort();
    void release();

    std::string device_;
    int baudrate_;
    int timeout_ms_;
    modbus_t* ctx;
    /** 串行化全部公共 IO，防止多线程并发读写同一个 modbus ctx / 同一串口 */
    mutable std::mutex io_mutex_;

    /** 重连退避：0=可立即重连；失败后 1s→2s→4s→…→30s 封顶（对齐 ModbusTCP）。
     *  串口失效时不每轮反复 modbus_connect，避免触发定制 libmodbus 的 RS485 断言崩溃。 */
    int reconnectDelayMs_ = 0;
    std::chrono::steady_clock::time_point nextReconnectAt_{};

    /** 硬错误滞后：连续达到 kHardErrorThreshold 次硬错误才真正 release+重连，
     *  单次瞬态错误（线路噪声/偶发 CRC）不触发，避免反复 modbus_connect 撞 libmodbus 断言。 */
    int hardErrorCount_ = 0;
    void noteHardError();   // 硬错误计数；达阈值才释放
    void noteIoSuccess();   // 一次成功 IO 清零计数
};

/**
 * Modbus TCP 传输。
 * 实现 IModbusBus；读写前 ensureConnected，失败后断链并按指数退避重连。
 * PCS、BAMU/BMS/空调等 TCP 设备共用此类。
 * 线程安全：每个公共方法由内置 io_mutex_ 串行化，同一实例可被多线程安全共享。
 */
class ModbusTCP : public IModbusBus {
public:
    /**
     * @param ip        设备 IP
     * @param port      端口，通常 502
     * @param slave_id  从站号（TCP 网关侧仍可能用到）
     * @param timeout_ms 应答超时（毫秒）；0 表示默认 5000ms
     */
    ModbusTCP(std::string ip, int port, int slave_id, int timeout_ms = 0);
    ~ModbusTCP() override;

    bool setSlave(int slave_id) override;

    bool isConnected() const override;

    int readRegisters(int addr, int num_reg, uint16_t* dest) override;
    int writeRegister(int addr, uint16_t value, const std::string& tableName) override;
    int writeCoil(int addr, bool value, const std::string& tableName) override;
    int writeRegisters(int addr, const uint16_t* values, int count, const std::string& tableName) override;  // FC16
    int writeBits(int addr, const uint8_t* values, int count, const std::string& tableName) override;        // FC15
    int readInputRegisters(int addr, int num_reg, uint16_t* dest) override;
    bool probe(int addr, int num_reg, uint16_t* dest, const std::string& tag) override;
    int readBits(int addr, int num_reg, uint8_t* dest) override;
    int readInputBits(int addr, int num_reg, uint8_t* dest) override;
    void disconnect();

private:
    /** socket 有效且未被对端关闭则复用，否则按退避窗口尝试重连 */
    bool ensureConnected();
    /** 建立 TCP 连接并设置从站/超时 */
    bool try_connect();
    /** poll 检测 socket 是否已挂死（POLLERR/POLLHUP/POLLRDHUP），避免“假活”白等超时 */
    bool check_connection() const;
    void create_context();
    /** 非阻塞 connect + 超时；成功返回已恢复阻塞模式的 fd，失败返回 -1 */
    int connectWithTimeout(const std::string& ip, int port, int timeout_ms) const;
    /** 关闭 socket，不加锁；仅供公共方法持锁路径与析构内部调用 */
    void disconnectUnlocked();

    std::string ip_;
    int port_;
    int slave_id_;
    int timeout_ms_;
    modbus_t* ctx_;
    /** 串行化全部公共 IO；isConnected/check_connection 也读 ctx_，故为 mutable */
    mutable std::mutex io_mutex_;

    /** 重连退避：0=可立即重连；失败后 1s→2s→4s→…→30s 封顶 */
    int reconnectDelayMs_ = 0;
    std::chrono::steady_clock::time_point nextReconnectAt_{};
};

#endif // MODBUSRTU_H
