#ifndef DIDO_H
#define DIDO_H

#include "ModbusPollEngine.h"
#include "ModbusRtu.h"
#include "MySQLDB_1.h"
#include <memory>
#include <string>

class Dido {
public:
    explicit Dido(const std::string& configPath = std::string());
    ~Dido();
    void didoThread(MySQLConnectionPool& pool);

private:
    void onAfterDecode(ModbusPollEngine& eng, MySQLDatabase& db);
    void writeDoIfChanged(int reg, bool desired, bool& actual, const char* name);

    std::unique_ptr<ModbusTCP> bus_;
    std::unique_ptr<ModbusPollEngine> engine_;
    bool yellowLed_ = false;
    bool greenLed_ = false;
    bool redLed_ = false;
    bool chiFaSwitch_ = false;
    bool qacSplit_ = false;
    bool readYellow_ = false;
    bool readGreen_ = false;
    bool readRed_ = false;
    bool readChiFa_ = false;
    bool readQac_ = false;
    uint16_t arr_[8]{};
};

#endif
