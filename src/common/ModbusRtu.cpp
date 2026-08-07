#include "ModbusRtu.h"
#include <thread>

void delay_modbus(int milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

// 构造函数
ModbusRTU::ModbusRTU(const char* device, int baudrate) {
    ctx = modbus_new_rtu(device, baudrate, 'N', 8, 1);
    modbus_connect(ctx);
    if (ctx == nullptr) {
        std::cerr << "Unable to allocate libmodbus context" << std::endl;
    }
    //std::cout << "open modbus" << std::endl;
}

// 析构函数
ModbusRTU::~ModbusRTU() {
    disconnect();
    //std::cout << "close modbus" << std::endl;
}

// 连接到 Modbus 从站
bool ModbusRTU::connect(int slave_id) {
    modbus_set_slave(ctx, slave_id);
    // if (modbus_connect(ctx) == -1) {
    //     std::cerr << "Connection failed: " << modbus_strerror(errno) << std::endl;
    //     return false;
    // }
    return true;
}

// 断开连接
void ModbusRTU::disconnect() {
    if (ctx) {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = nullptr;
    }
}

// 读取保持寄存器
int ModbusRTU::readRegisters(int addr, int num_reg, uint16_t* dest) {
    int rc = modbus_read_registers(ctx, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr <<std::endl;
    }
    return rc;
}

// 写入保持寄存器
int ModbusRTU::writeRegister(int addr, uint16_t value, const std::string& tableName) {
    int rc = modbus_write_register(ctx, addr, value);
    if (rc == -1) {
        std::cerr << tableName << "Write error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }else {
        std::cout << tableName << " Write success! addr: " << addr << " value: " << value << std::endl;
    }
    return rc;
}

// 读取输入寄存器
int ModbusRTU::readInputRegisters(int addr, int num_reg, uint16_t* dest) {
    int rc = modbus_read_input_registers(ctx, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}

// 读线圈（数据需要转换为static_cast<int>）
int ModbusRTU::readBits(int addr, int num_reg, uint8_t *dest) {
    int rc = modbus_read_bits(ctx, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}

// 读离散寄存器（数据需要转换为static_cast<int>）
int ModbusRTU::readInputBits(int addr, int num_reg, uint8_t *dest) {
    int rc = modbus_read_input_bits(ctx, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}

// 测试通信状态
int ModbusRTU::commTest(int addr, int num_reg, uint16_t* dest, const std::string& tableName) {
    int rc = modbus_read_registers(ctx, addr, num_reg, dest);
    if (rc == -1) {
        std::cout << "disconnect: " << tableName << std::endl;
        return -1;
    }
    return 0;
}



// 构造函数
ModbusTCP::ModbusTCP(std::string ip, int port, int slave_id)
        : ip_(std::move(ip)), port_(port), slave_id_(slave_id),
          ctx_(nullptr) {}

// 析构函数
ModbusTCP::~ModbusTCP(){
    disconnect();
}

// 安全读操作（包含连接状态检查）
bool ModbusTCP::commTest(int addr, int num, uint16_t* dest, const std::string& tableName) {
    while(true) {
        if (check_connection() || try_connect()) {
            int rc = modbus_read_registers(ctx_, addr, num, dest);
            if (rc == num)
            {
                return true;
            }
            else {
                std::cout << "错误码: "<< errno << " 表名: "<< tableName <<std::endl;
                std::cerr << "连接异常，启动重连..." << std::endl;
                disconnect();
                return false;
            }
        } else {
            return false;
        }
    }
}

bool ModbusTCP::commTest(int addr, int num, uint8_t* dest, const std::string& tableName) {
    while(true) {
        if (check_connection() || try_connect()) {
            int rc = modbus_read_input_bits(ctx_, addr, num, dest);
            if (rc == num)
            {
                return true;
            }
            else {
                std::cout << "错误码: "<< errno << " 表名: "<< tableName <<std::endl;
                std::cerr << "连接异常，启动重连..." << std::endl;
                disconnect();
                return false;
            }
        } else {
            return false;
        }
    }
}

// 无限重连核心逻辑
bool ModbusTCP::try_connect() {
    disconnect();
    create_context();

    while(true) { // 无限重试循环
        if (modbus_connect(ctx_) == 0) {
            std::cout << "成功连接到 " << ip_ << std::endl;
            return true;
        }
        std::cerr << "连接失败: " << modbus_strerror(errno) << std::endl;
        return false;
        delay_modbus(1000);
    }
}

// 连接状态检查
bool ModbusTCP::check_connection() {
    return ctx_ && modbus_get_socket(ctx_) != -1;
}

// 创建新上下文
void ModbusTCP::create_context() {
    if (ctx_) modbus_free(ctx_);
    ctx_ = modbus_new_tcp(ip_.c_str(), port_);
    if (ctx_) {
        modbus_set_slave(ctx_, slave_id_);
        modbus_set_response_timeout(ctx_, 5, 0);
    }
}

void ModbusTCP::disconnect() {
    if (ctx_) {
        modbus_close(ctx_);
        // 保持上下文对象用于重连
    }
}

// 读取保持寄存器
int ModbusTCP::readRegisters(int addr, int num_reg, uint16_t* dest) {
    int rc = modbus_read_registers(ctx_, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr <<std::endl;
    }
    return rc;
}

// 写入保持寄存器
int ModbusTCP::writeRegister(int addr, uint16_t value, const std::string& tableName) {
    int rc = modbus_write_register(ctx_, addr, value);
    if (rc == -1) {
        std::cerr << tableName << "Write error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }else {
        std::cout << tableName << " Write success! addr: " << addr << " value: " << value << std::endl;
    }
    return rc;
}

// 读取输入寄存器
int ModbusTCP::readInputRegisters(int addr, int num_reg, uint16_t* dest) {
    int rc = modbus_read_input_registers(ctx_, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}

// 读线圈（数据需要转换为static_cast<int>）
int ModbusTCP::readBits(int addr, int num_reg, uint8_t *dest) {
    int rc = modbus_read_bits(ctx_, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}

// 读离散寄存器（数据需要转换为static_cast<int>）
int ModbusTCP::readInputBits(int addr, int num_reg, uint8_t *dest) {
    int rc = modbus_read_input_bits(ctx_, addr, num_reg, dest);
    if (rc == -1) {
        std::cerr << "Read error: " << modbus_strerror(errno) << " addr: "<< addr  << std::endl;
    }
    return rc;
}