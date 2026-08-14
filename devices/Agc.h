#ifndef AGC_H
#define AGC_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <array>
#include <memory>
#include <string>

/**
 * 柴发 AGC：单 RTU 总线多从站（最多 4 台），逐台轮询。
 * 连接/点表来自 config/modbus/devices.json + templates/agc.json。
 * 并机台数 qt 地址在 devices.json 的 count_qt_addr（现场按 EMS 点表改）；com_alarm 16~19。
 */
class Agc {
public:
    static constexpr std::size_t kMaxGensets = 4;   // 柴发最大台数
    static constexpr int kComAlarmAddrBase = 16;    // com_alarm：柴发 16~19

    explicit Agc(const std::string& configPath = std::string());
    ~Agc();
    void runAgcThread(MySQLConnectionPool& pool);

private:
    void pollOneGenset(MySQLDatabase& db, int amount);
    void sleepFrameGap() const;

    std::unique_ptr<ModbusRTU> bus_;
    std::unique_ptr<ModbusPollEngine> engine_;
    std::array<int, kMaxGensets> commErrCount_{};
    uint16_t probeBuf_ = 0;
};

#endif  // AGC_H
