#ifndef MODBUSRTU_H
#define MODBUSRTU_H

#include <modbus.h>
#include <iostream>

class ModbusRTU {
public:
    ModbusRTU(const char* device, int baudrate);
    ~ModbusRTU();

    bool connect(int slave_id);
    void disconnect();

    int readRegisters(int addr, int num_reg, uint16_t* dest);
    int writeRegister(int addr, uint16_t value, const std::string& tableName);
    int readInputRegisters(int addr, int num_reg, uint16_t* dest);
    int commTest(int addr, int num_reg, uint16_t* dest, const std::string& tableName);
    int readBits(int addr, int num_reg, uint8_t* dest);
    int readInputBits(int addr, int num_reg, uint8_t* dest);

private:
    modbus_t* ctx;
};

class ModbusTCP {
public:
    ModbusTCP(std::string ip, int port, int slave_id);
    ~ModbusTCP();

    int readRegisters(int addr, int num_reg, uint16_t* dest);
    int writeRegister(int addr, uint16_t value, const std::string& tableName);
    int readInputRegisters(int addr, int num_reg, uint16_t* dest);
    bool commTest(int addr, int num, uint16_t* dest, const std::string& tableName);
    bool commTest(int addr, int num, uint8_t* dest, const std::string& tableName);
    int readBits(int addr, int num_reg, uint8_t* dest);
    int readInputBits(int addr, int num_reg, uint8_t* dest);
    void disconnect();

private:
    bool try_connect();
    bool check_connection();
    void create_context();
    

    modbus_t* ctx_;
    std::string ip_;
    int port_;
    int slave_id_;
};

#endif //MODBUSRTU_H
