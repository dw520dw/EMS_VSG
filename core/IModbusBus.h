#ifndef I_MODBUS_BUS_H
#define I_MODBUS_BUS_H

#include <cstdint>
#include <string>

/** Modbus function code used by config-driven poll blocks. */
enum class ModbusFc : int {
    Coils = 1,
    DiscreteInputs = 2,
    HoldingRegisters = 3,
    InputRegisters = 4
};

/**
 * Unified Modbus bus interface for RTU/TCP.
 * Device poll engine depends only on this; concrete transports keep existing APIs.
 *
 * 线程安全约定：实现类不内置互斥，同一实例须由单个线程串行访问
 * （BAMU 的多个引擎共享一个 TCP 连接、DIDO 共享一个 RTU 时均为顺序调用）。
 */
class IModbusBus {
public:
    virtual ~IModbusBus() = default;

    /** Select slave id (RTU). TCP may ignore or re-apply slave. */
    virtual bool setSlave(int slave_id) = 0;

    /** 传输是否已连接：RTU=串口已打开，TCP=socket 有效且未挂死。 */
    virtual bool isConnected() const = 0;

    virtual int readRegisters(int addr, int num_reg, uint16_t* dest) = 0;
    virtual int readInputRegisters(int addr, int num_reg, uint16_t* dest) = 0;
    virtual int readBits(int addr, int num_reg, uint8_t* dest) = 0;
    virtual int readInputBits(int addr, int num_reg, uint8_t* dest) = 0;
    virtual int writeRegister(int addr, uint16_t value, const std::string& tableName) = 0;
    virtual int writeCoil(int addr, bool value, const std::string& tableName) = 0;
    /** FC16 写多保持寄存器；成功返回写入数量 count，失败返回 -1 */
    virtual int writeRegisters(int addr, const uint16_t* values, int count, const std::string& tableName) = 0;
    /** FC15 写多线圈；成功返回写入数量 count，失败返回 -1 */
    virtual int writeBits(int addr, const uint8_t* values, int count, const std::string& tableName) = 0;

    /** Lightweight link probe; true = OK. */
    virtual bool probe(int addr, int num_reg, uint16_t* dest, const std::string& tag) = 0;

    /** Read a block by function code into register or bit buffer. Returns -1 on failure. */
    int readBlock(ModbusFc fc, int addr, int count, uint16_t* regBuf, uint8_t* bitBuf)
    {
        switch (fc)
        {
        case ModbusFc::HoldingRegisters:
            return readRegisters(addr, count, regBuf);
        case ModbusFc::InputRegisters:
            return readInputRegisters(addr, count, regBuf);
        case ModbusFc::Coils:
            return readBits(addr, count, bitBuf);
        case ModbusFc::DiscreteInputs:
            return readInputBits(addr, count, bitBuf);
        }
        return -1;
    }
};

#endif  // I_MODBUS_BUS_H
