#ifndef TEST_MYSQLDB_H
#define TEST_MYSQLDB_H

#include "IDataSink.h"
#include "mysql.h"
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <stdexcept>
#include <string>
#include <iostream>
#include <list>
#include <fstream>
#include <map>
#include <array>
#include <cmath>
#include <utility>
#include "logger.h"

constexpr double kDbValueEpsilon = 1e-4;

bool dbValueChanged(double previous, double current, double epsilon = kDbValueEpsilon);

/** 表名/列名转义：反引号包裹并转义内部反引号。
 *  值一律为数字（std::to_string 生成），无需转义；表名来自受信配置，转义作纵深防御。 */
inline std::string mysqlEscapeIdent(const std::string& name)
{
    std::string out;
    out.reserve(name.size() + 2);
    out.push_back('`');
    for (char c : name)
    {
        if (c == '`')
        {
            out.append("``");
        }
        else
        {
            out.push_back(c);
        }
    }
    out.push_back('`');
    return out;
}

class MySQLConnectionPool {
public:
    explicit MySQLConnectionPool(const char* host, const char* user, const char* password, const char* database, int poolSize);
    ~MySQLConnectionPool();
    MYSQL* getConnection();
    void releaseConnection(MYSQL* conn);

private:
    std::vector<MYSQL*> connections;
    std::queue<MYSQL*> availableConnections;
    std::mutex mutex;
    std::condition_variable condition;
    int maxSize;
};

class MySQLDatabase : public IDataSink {
public:
    explicit MySQLDatabase(MySQLConnectionPool& pool);
    ~MySQLDatabase() override;

    // ---- IDataSink：供 ModbusPollEngine 等只依赖抽象接口的调用方使用 ----
    void updateRealtime(const std::string& table,
                        const std::vector<std::pair<int, double>>& points) override;
    void updateValue(int addr, double value, const std::string& table) override;
    int readInt(int addr, const std::string& table) override;
    double readFloat(int addr, const std::string& table) override;
    std::map<int, float> readBatch(const std::string& table,
                                   const std::vector<int>& addrs) override;

    void createTable(const std::string& tableName);
    void insert(const std::string& sql);
    void update(int addr, int newValue, const std::string& tableName);
    void update(int addr, double newValue, const std::string& tableName);
    int select(int addr, const std::string& tableName);
    double select_float(int addr, const std::string& tableName);
    void deleteOldRows(const std::string& sourceTableName, int count);
    /** @deprecated 拼写错误的历史别名，新代码请用 deleteOldRows */
    void delect(const std::string& sourceTableName, int count) { deleteOldRows(sourceTableName, count); }
    std::map<int, float> selectAllData(const std::string& tableName, int count);
    std::map<int, float> selectMultipleData(const std::string& tableName, const std::vector<int>& addrs);
    /**
     * 读实时表全部行的 EN + value（历史上传用）。
     * 约定：表结构含 EN/value，通常一行一 addr。
     */
    std::vector<std::pair<std::string, double>> selectAllEnValues(const std::string& tableName);
    void checkAndCleanTable(const std::string& tableName, double maxSizeMB);
    void copyTable(const std::string& sourceTableName, const std::string& targetTableName);
    void resetAllValuesToZeroExceptFirst(const std::string& tableName);


private:
    MYSQL* conn;
    MySQLConnectionPool& pool;
};
// 将采集到的modbus数据放进列表，拼接成SQL语句
class databaseList {
public:
    struct modbusData {
        int addr;
        double value;
        modbusData(const int a,  const double v) :
                addr(a), value(v) {}
    };
    void clearData() {
        modbusDatas.clear();
    }
    void addData(int addr, int value)  {
        modbusDatas.emplace_back(addr, value);
    }
    void addData(int addr, double value)  {
        modbusDatas.emplace_back(addr, value);
    }
    void addData(int addr, uint32_t value)  {
        modbusDatas.emplace_back(addr, value);
    }

    std::string spliceData(const std::string& tableName) {
        if (modbusDatas.empty()) {
            return {};
        }
        const std::string table = mysqlEscapeIdent(tableName);
        std::string sql = "UPDATE " + table + " set value = case";
        std::string whereClause = " WHERE addr IN (";
        bool first = true;
        for (const auto& modbusData : modbusDatas){
            sql = sql + " when addr = " + std::to_string(modbusData.addr) + " then " + std::to_string(modbusData.value);
            if (!first) whereClause += ",";
            whereClause += std::to_string(modbusData.addr);
            first = false;
        }
        sql = sql + " end, time = now()" + whereClause + ");";
        return sql;
    }

private:
    std::list<modbusData> modbusDatas;
};

/** 增量写库：值相对上次快照变化时才加入 batch update 列表 */
template <size_t N>
inline void addDbDataIfChanged(databaseList& list, int addr, double value,
                               std::array<double, N>& lastValues, bool& hasSnapshot,
                               double epsilon = kDbValueEpsilon)
{
    const size_t idx = static_cast<size_t>(addr);
    if (idx >= N) {
        return;
    }
    if (!hasSnapshot || dbValueChanged(lastValues[idx], value, epsilon)) {
        list.addData(addr, value);
        lastValues[idx] = value;
    }
}

#endif // TEST_MYSQLDB_H

