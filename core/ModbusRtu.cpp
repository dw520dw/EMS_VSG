/**
 * Modbus RTU / TCP 传输实现（libmodbus）。
 * 对上层统一暴露 IModbusBus；TCP 侧读写前 ensureConnected，失败后断链按指数退避重连；
 * RTU 侧按需连接，串口失效时读写前自动重开。
 */

#include "ModbusRtu.h"
#include "logger.h"
#include <algorithm>
#include <cerrno>
#include <poll.h>

// POSIX 网络：非阻塞 connect 用（arm 目标为 Linux；本机语法检查用 stub 头）
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

constexpr int kReconnectInitMs = 1000;
constexpr int kReconnectMaxMs = 30000;
/** 非阻塞 connect 超时（毫秒）：对不可达 IP 不再卡 OS 默认超时 */
constexpr int kConnectTimeoutMs = 2000;
/** 硬错误滞后阈值：连续达到该次数才 release+重连，单次瞬态错误不触发 */
constexpr int kHardErrorThreshold = 3;

/**
 * 软错误：连接本身仍健康，不应触发断链重连。
 * - ETIMEDOUT：设备响应慢/应答超时（读失败最常见，慢设备每轮都该保留连接）
 * - EBUSY：从站忙（异常码 6）、EAGAIN：异常码 11（slave failure）
 * 其余 errno 视为硬错误（对端重置/断链/串口失效），需断链走重连。
 */
bool isSoftError(int e)
{
    return e == ETIMEDOUT || e == EBUSY || e == EAGAIN || e == EINPROGRESS;
}

}  // namespace

// ============================== ModbusRTU ==============================

ModbusRTU::ModbusRTU(const char* device, int baudrate, int timeout_ms)
    : device_(device),
      baudrate_(baudrate),
      timeout_ms_(timeout_ms > 0 ? timeout_ms : 1000),
      ctx(nullptr)
{
    // 按需连接：openPort 失败不抛出，后续读写前 ensureConnected 会重试
    openPort();
}

ModbusRTU::~ModbusRTU()
{
    release();
}

void ModbusRTU::release()
{
    if (ctx)
    {
        modbus_close(ctx);
        modbus_free(ctx);
        ctx = nullptr;
    }
}

void ModbusRTU::noteIoSuccess()
{
    hardErrorCount_ = 0;
}

void ModbusRTU::noteHardError()
{
    ++hardErrorCount_;
    if (hardErrorCount_ >= kHardErrorThreshold)
    {
        LOG_ACTION("串口连续硬错误 " << hardErrorCount_ << " 次，释放 ctx 准备重连");
        hardErrorCount_ = 0;
        release();
    }
}

bool ModbusRTU::openPort()
{
    // 设备文件不存在（USB 转串口松脱/未上电、/usr/dev/serial 链接失效）时绝不进 libmodbus：
    // 定制版 libmodbus 在 RTU connect 内部启用 RS485（modbus_rtu_set_serial_mode），
    // 对失效串口可能触发 assert → SIGABRT 把整个进程带崩。
    if (::access(device_.c_str(), F_OK) != 0)
    {
        LOG_ACTION("ModbusRTU 串口设备不存在: " << device_);
        return false;
    }
    // 8 数据位、无校验、1 停止位
    ctx = modbus_new_rtu(device_.c_str(), baudrate_, 'N', 8, 1);
    if (ctx == nullptr)
    {
        LOG_ACTION("ModbusRTU 创建上下文失败: " << device_);
        return false;
    }
    // 串口丢帧/协议错乱时自动清状态并重开端口
    modbus_set_error_recovery(ctx, static_cast<modbus_error_recovery_mode>(
                                       MODBUS_ERROR_RECOVERY_LINK | MODBUS_ERROR_RECOVERY_PROTOCOL));
    modbus_set_response_timeout(ctx, timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000);

    if (modbus_connect(ctx) != 0)
    {
        LOG_ACTION("ModbusRTU 打开串口失败: " << device_ << " ("
                   << modbus_strerror(errno) << ")");
        modbus_free(ctx);
        ctx = nullptr;
        return false;
    }
    return true;
}

bool ModbusRTU::ensureConnected()
{
    if (ctx && modbus_get_socket(ctx) != -1)
    {
        reconnectDelayMs_ = 0;  // 连接健康，清零退避
        return true;
    }

    // 退避窗口内快速失败：串口失效时不要每轮(600ms)都反复 modbus_connect，
    // 既耗 CPU/总线，也放大定制 libmodbus 在 connect 内部断言崩溃的窗口。
    const auto now = std::chrono::steady_clock::now();
    if (reconnectDelayMs_ > 0 && now < nextReconnectAt_)
    {
        return false;
    }

    // 串口未打开或已失效（USB 转串口重枚举等）：重新打开
    release();
    if (openPort())
    {
        reconnectDelayMs_ = 0;
        hardErrorCount_ = 0;  // 重连成功，清零硬错误计数
        return true;
    }

    // 指数退避：1s -> 2s -> 4s -> ... -> 30s 封顶
    reconnectDelayMs_ =
        (reconnectDelayMs_ == 0) ? kReconnectInitMs
                                 : std::min(reconnectDelayMs_ * 2, kReconnectMaxMs);
    nextReconnectAt_ = now + std::chrono::milliseconds(reconnectDelayMs_);
    return false;
}

bool ModbusRTU::setSlave(int slave_id)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return false;
    }
    modbus_set_slave(ctx, slave_id);
    return true;
}

void ModbusRTU::disconnect()
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    release();
}

bool ModbusRTU::isConnected() const
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    return ctx && modbus_get_socket(ctx) != -1;
}

int ModbusRTU::readRegisters(int addr, int num_reg, uint16_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_registers(ctx, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();  // 连续硬错误达阈值才释放重连
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::writeRegister(int addr, uint16_t value, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_register(ctx, addr, value);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " Write error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::writeCoil(int addr, bool value, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_bit(ctx, addr, value ? TRUE : FALSE);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " Coil write error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::writeRegisters(int addr, const uint16_t* values, int count, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_registers(ctx, addr, count, values);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " WriteRegisters error: " << modbus_strerror(e) << " addr: " << addr
                             << " count: " << count);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::writeBits(int addr, const uint8_t* values, int count, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_bits(ctx, addr, count, values);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " WriteBits error: " << modbus_strerror(e) << " addr: " << addr
                             << " count: " << count);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::readInputRegisters(int addr, int num_reg, uint16_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_input_registers(ctx, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::readBits(int addr, int num_reg, uint8_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_bits(ctx, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

int ModbusRTU::readInputBits(int addr, int num_reg, uint8_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_input_bits(ctx, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            noteHardError();
        }
    }
    else
    {
        noteIoSuccess();
    }
    return rc;
}

bool ModbusRTU::probe(int addr, int num_reg, uint16_t* dest, const std::string& tag)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return false;
    }
    // 任意读失败（超时/CRC/从站异常等）均判为探测失败；软错误保留串口复用
    const int rc = modbus_read_registers(ctx, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("probe fail: " << tag << " (" << modbus_strerror(e) << ")");
        if (!isSoftError(e))
        {
            noteHardError();
        }
        return false;
    }
    noteIoSuccess();
    return true;
}

// ============================== ModbusTCP ==============================

ModbusTCP::ModbusTCP(std::string ip, int port, int slave_id, int timeout_ms)
    : ip_(std::move(ip)),
      port_(port),
      slave_id_(slave_id),
      timeout_ms_(timeout_ms > 0 ? timeout_ms : 5000),
      ctx_(nullptr)
{
    // 延迟到首次读写时再 connect，避免构造期阻塞
}

ModbusTCP::~ModbusTCP()
{
    // 析构期不持锁：若还有线程在并发使用该对象，本就是未定义行为，持锁只会死等
    disconnectUnlocked();
    if (ctx_)
    {
        modbus_free(ctx_);
        ctx_ = nullptr;
    }
}

bool ModbusTCP::setSlave(int slave_id)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    slave_id_ = slave_id;
    if (ctx_)
    {
        modbus_set_slave(ctx_, slave_id_);
    }
    return true;
}

bool ModbusTCP::isConnected() const
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    return check_connection();
}

bool ModbusTCP::ensureConnected()
{
    if (check_connection())
    {
        reconnectDelayMs_ = 0;  // 连接健康，清零退避
        return true;
    }

    // 退避窗口内快速失败，避免对不可达 IP 反复阻塞 connect
    const auto now = std::chrono::steady_clock::now();
    if (reconnectDelayMs_ > 0 && now < nextReconnectAt_)
    {
        return false;
    }

    if (try_connect())
    {
        reconnectDelayMs_ = 0;
        return true;
    }

    // 指数退避：1s -> 2s -> 4s -> ... -> 30s 封顶
    reconnectDelayMs_ =
        (reconnectDelayMs_ == 0) ? kReconnectInitMs
                                 : std::min(reconnectDelayMs_ * 2, kReconnectMaxMs);
    nextReconnectAt_ = now + std::chrono::milliseconds(reconnectDelayMs_);
    return false;
}

bool ModbusTCP::probe(int addr, int num_reg, uint16_t* dest, const std::string& tag)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return false;
    }
    const int rc = modbus_read_registers(ctx_, addr, num_reg, dest);
    if (rc == num_reg)
    {
        return true;
    }
    const int e = errno;
    LOG_ACTION("probe fail: " << tag << " errno=" << e << " (" << modbus_strerror(e) << ")"
                              << (isSoftError(e) ? "，软错误保留连接" : "，将重连"));
    if (!isSoftError(e))
    {
        disconnectUnlocked();
    }
    return false;
}

bool ModbusTCP::try_connect()
{
    disconnectUnlocked();
    create_context();
    if (!ctx_)
    {
        return false;
    }

    // 非阻塞 connect + 超时：对不可达 IP 不再卡 OS 默认超时（数十秒），超时后走指数退避
    const int fd = connectWithTimeout(ip_, port_, kConnectTimeoutMs);
    if (fd < 0)
    {
        LOG_ACTION("连接失败: " << ip_ << ":" << port_ << " (" << modbus_strerror(errno) << ")");
        modbus_free(ctx_);
        ctx_ = nullptr;
        return false;
    }
    // 把已连好的 socket 交给 libmodbus；不再调 modbus_connect（它会自建 socket 重复 connect）
    modbus_set_socket(ctx_, fd);
    std::cout << "成功连接到 " << ip_ << ":" << port_ << std::endl;
    return true;
}

int ModbusTCP::connectWithTimeout(const std::string& ip, int port, int timeout_ms) const
{
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -1;
    }

    // 置非阻塞使 connect 立即返回；之后用 poll 等可写并检查 SO_ERROR
    const int saved_flags = ::fcntl(fd, F_GETFL, 0);
    if (saved_flags < 0)
    {
        ::close(fd);
        return -1;
    }
    ::fcntl(fd, F_SETFL, saved_flags | O_NONBLOCK);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::inet_pton(AF_INET, ip.c_str(), &addr.sin_addr) != 1)
    {
        errno = EINVAL;
        ::close(fd);
        return -1;
    }

    int rc = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (rc == 0)
    {
        ::fcntl(fd, F_SETFL, saved_flags);  // 立即成功，恢复阻塞
        return fd;
    }
    if (errno != EINPROGRESS)
    {
        ::close(fd);
        return -1;
    }

    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLOUT;
    const int prc = ::poll(&pfd, 1, timeout_ms);
    if (prc <= 0 || (pfd.revents & POLLOUT) == 0)
    {
        if (prc == 0)
        {
            errno = ETIMEDOUT;  // 超时也要有明确 errno 供日志
        }
        ::close(fd);
        return -1;
    }

    // POLLOUT 可能同时由对端拒绝触发；以 SO_ERROR 为准
    int soerr = 0;
    socklen_t soerr_len = sizeof(soerr);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) != 0 || soerr != 0)
    {
        if (soerr != 0)
        {
            errno = soerr;
        }
        ::close(fd);
        return -1;
    }

    ::fcntl(fd, F_SETFL, saved_flags);  // 恢复阻塞，后续 libmodbus 的同步 IO 正常工作
    return fd;
}

bool ModbusTCP::check_connection() const
{
    if (!ctx_)
    {
        return false;
    }
    const int fd = modbus_get_socket(ctx_);
    if (fd == -1)
    {
        return false;
    }
    // 0 超时 poll：只探测对端是否已关闭/出错（POLLHUP/POLLERR），不做真实读写，
    // 避免“假活”socket 上白等整个应答超时
    pollfd pfd;
    pfd.fd = fd;
    pfd.events = 0;
    pfd.revents = 0;
    const int rc = ::poll(&pfd, 1, 0);
    if (rc < 0)
    {
        return false;
    }
    return rc == 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0;
}

void ModbusTCP::create_context()
{
    if (ctx_)
    {
        modbus_free(ctx_);
        ctx_ = nullptr;
    }
    ctx_ = modbus_new_tcp(ip_.c_str(), port_);
    if (ctx_)
    {
        modbus_set_slave(ctx_, slave_id_);
        // 应答超时可配置，避免掉线时每个读请求空等
        modbus_set_response_timeout(ctx_, timeout_ms_ / 1000, (timeout_ms_ % 1000) * 1000);
    }
}

void ModbusTCP::disconnect()
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    disconnectUnlocked();
}

void ModbusTCP::disconnectUnlocked()
{
    if (ctx_)
    {
        modbus_close(ctx_);
    }
}

int ModbusTCP::readRegisters(int addr, int num_reg, uint16_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_registers(ctx_, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();  // 硬错误：断链，下轮 ensureConnected 重连
        }
    }
    return rc;
}

int ModbusTCP::writeRegister(int addr, uint16_t value, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_register(ctx_, addr, value);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " Write error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::writeCoil(int addr, bool value, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_bit(ctx_, addr, value ? TRUE : FALSE);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " Coil write error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::writeRegisters(int addr, const uint16_t* values, int count, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_registers(ctx_, addr, count, values);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " WriteRegisters error: " << modbus_strerror(e) << " addr: " << addr
                             << " count: " << count);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::writeBits(int addr, const uint8_t* values, int count, const std::string& tableName)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_write_bits(ctx_, addr, count, values);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION(tableName << " WriteBits error: " << modbus_strerror(e) << " addr: " << addr
                             << " count: " << count);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::readInputRegisters(int addr, int num_reg, uint16_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_input_registers(ctx_, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::readBits(int addr, int num_reg, uint8_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_bits(ctx_, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}

int ModbusTCP::readInputBits(int addr, int num_reg, uint8_t* dest)
{
    std::lock_guard<std::mutex> lock(io_mutex_);
    if (!ensureConnected())
    {
        return -1;
    }
    int rc = modbus_read_input_bits(ctx_, addr, num_reg, dest);
    if (rc == -1)
    {
        const int e = errno;
        LOG_ACTION("Read error: " << modbus_strerror(e) << " addr: " << addr);
        if (!isSoftError(e))
        {
            disconnectUnlocked();
        }
    }
    return rc;
}
