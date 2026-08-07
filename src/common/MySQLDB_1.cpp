#include "MySQLDB_1.h"
#include <chrono>
#include <cctype>
#include <cmath>

namespace {

bool isSkippableInsertSql(const std::string& sql)
{
    if (sql.empty()) {
        return true;
    }
    for (unsigned char ch : sql) {
        if (!std::isspace(ch)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool dbValueChanged(double previous, double current, double epsilon)
{
    return std::abs(previous - current) > epsilon;
}

// 连接池
MySQLConnectionPool::MySQLConnectionPool(const char* host, const char* user, const char* password, const char* database, int poolSize)
        : maxSize(poolSize) {
    for (int i = 0; i < poolSize; ++i) {
        MYSQL* conn = mysql_init(nullptr);
        if (conn == nullptr || mysql_real_connect(conn, host, user, password, database, 3306, nullptr, 0) == nullptr) {
            mysql_close(conn);
            throw std::runtime_error("Failed to create connection: " + std::string(mysql_error(conn)));
        }
        connections.push_back(conn);
        availableConnections.push(conn);
    }
}

MySQLConnectionPool::~MySQLConnectionPool() {
    for (MYSQL* conn : connections) {
        mysql_close(conn);
    }
}

MYSQL* MySQLConnectionPool::getConnection() {
    std::unique_lock<std::mutex> lock(mutex);
    // 避免无限等待：若连接池被占满（如某处未归还连接），超时后抛异常便于定位，而不是永久卡死
    constexpr auto timeout = std::chrono::seconds(30);
    if (!condition.wait_for(lock, timeout, [this]() { return !availableConnections.empty(); })) {
        LOG_ACTION("MySQL连接池获取超时(30s)，可能存在连接未归还或池被占满");
        throw std::runtime_error("MySQL connection pool timeout: no available connection in 30s");
    }
    MYSQL* conn = availableConnections.front();
    availableConnections.pop();
    return conn;
}

void MySQLConnectionPool::releaseConnection(MYSQL* conn) {
    std::lock_guard<std::mutex> lock(mutex);
    availableConnections.push(conn);
    condition.notify_one();
}

MySQLDatabase::MySQLDatabase(MySQLConnectionPool& pool)
        : pool(pool), conn(pool.getConnection()) {
    if (conn == nullptr) {
        LOG_ACTION("MySQL错误");
        throw std::runtime_error("Failed to obtain a connection from the pool.");
    }
    //std::cout << "Open db" << std::endl;
}

MySQLDatabase::~MySQLDatabase() {
    pool.releaseConnection(conn); // 归还连接到连接池
    //std::cout << "close db" << std::endl;
}


// 创建表
void MySQLDatabase::createTable(const std::string& tableName) {
    std::string sql = "CREATE TABLE IF NOT EXISTS " + tableName + " ("
                                                                  "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                                                                  "addr INT NOT NULL,"
                                                                  "CN VARCHAR(100) NOT NULL,"
                                                                  "EN VARCHAR(100) NOT NULL,"
                                                                  "value FLOAT NOT NULL,"
                                                                  "time TIMESTAMP NOT NULL);";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION("MySQL错误");
        throw std::runtime_error("createTable failed: " + std::string(mysql_error(conn)));
    }
}

// 一次性插入多个数据（增量写库无变化时 spliceData 为空，直接跳过）
void MySQLDatabase::insert(const std::string& sql) {
    if (isSkippableInsertSql(sql)) {
        return;
    }
    if (mysql_query(conn, sql.c_str())) {
        const char* errMsg = mysql_error(conn);
        if (errMsg != nullptr && std::string(errMsg) == "Query was empty") {
            return;
        }
        LOG_ACTION("MySQL错误");
        throw std::runtime_error("insert failed: " + std::string(errMsg != nullptr ? errMsg : "unknown"));
    }
}

// 更新表里数据
void MySQLDatabase::update(int addr, int newValue, const std::string& tableName) {
    std::string sql = "UPDATE " + tableName +  " set value = " + std::to_string(newValue) + ", time = now() WHERE addr = " + std::to_string(addr) + ";";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (tableName) + " update failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " update failed: " + std::string(mysql_error(conn)));
    }
}

// 更新表里数据，重载函数
void MySQLDatabase::update(int addr, double newValue, const std::string& tableName) {
    std::string sql = "UPDATE " + tableName +  " set value = " + std::to_string(newValue) + ", time = now() WHERE addr = " + std::to_string(addr) + ";";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (tableName) + " update failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " update failed: " + std::string(mysql_error(conn)));
    }
}

// 查询表里最新插入的 value 的值
int MySQLDatabase::select(int addr, const std::string& tableName) {
    std::string sql = "SELECT value FROM " + tableName + " a WHERE (addr, time) IN (SELECT addr, MAX(time) FROM " + tableName + " GROUP BY addr) AND addr = " + std::to_string(addr) + ";";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (tableName) + " select failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " select failed: " + std::string(mysql_error(conn)));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        LOG_ACTION(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
    }
    int num_fields = mysql_num_fields(result);
    MYSQL_ROW row;
    int value = 0;
    while ((row = mysql_fetch_row(result))) {
        for (int i = 0; i < num_fields; i++) {
            value = std::atoi(row[i]);
        }
    }
    mysql_free_result(result);
    return value;
}

// 查询浮点数值
double MySQLDatabase::select_float(int addr, const std::string& tableName) {
    // std::string sql = "SELECT value FROM (SELECT *, row_number() OVER (PARTITION BY addr ORDER BY time DESC) AS "
    //                   "RN FROM " + tableName + " ) AS air_with_rn WHERE RN = 1 AND addr = " + std::to_string(addr) + ";";
    std::string sql = "SELECT value FROM " + tableName + " a WHERE (addr, time) IN (SELECT addr, MAX(time) FROM " + tableName + " GROUP BY addr) AND addr = " + std::to_string(addr) + ";";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (tableName) + " select_float failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " select_float failed: " + std::string(mysql_error(conn)));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        LOG_ACTION(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
    }
    int num_fields = mysql_num_fields(result);
    MYSQL_ROW row;
    float value = 0;
    while ((row = mysql_fetch_row(result))) {
        for (int i = 0; i < num_fields; i++) {
            value = std::atof(row[i]);
        }
    }
    mysql_free_result(result);
    return value;
}

// 删除其余数据，只保留通讯状态
void MySQLDatabase::delect(const std::string& sourceTableName, const int count) {
    // 删除所有数据的SQL语句
    std::string sql = "DELETE FROM " + sourceTableName + " WHERE id NOT IN "
                                                         "(SELECT id FROM (SELECT id FROM " + sourceTableName + " ORDER BY id DESC LIMIT " + std::to_string(count) + " ) AS temp);";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (sourceTableName) + " delect failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (sourceTableName) + " delect failed: " + std::string(mysql_error(conn)));
    }
}

std::map<int, float> MySQLDatabase::selectAllData(const std::string& tableName, int count){
    std::map<int, float> myMap;
    std::string sql = "SELECT addr, value FROM " + tableName + " ORDER BY id DESC LIMIT " + std::to_string(count);
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string (tableName) + " select failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " select failed: " + std::string(mysql_error(conn)));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        LOG_ACTION(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string (tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
    }

    int num_fields = mysql_num_fields(result);
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        for (int i = 0; i < num_fields; i++) {
            int addr = std::stoi(row[0]);
            float value = std::stof(row[1]);
            myMap[addr] = value;
        }
    }
    mysql_free_result(result);
    return myMap;
}

// 批量查询指定地址的数据
std::map<int, float> MySQLDatabase::selectMultipleData(const std::string& tableName, const std::vector<int>& addrs) {
    std::map<int, float> resultMap;

    if (addrs.empty()) {
        return resultMap;
    }

    try {
        // 构建 IN 查询语句
        std::string addrList = "";
        for (size_t i = 0; i < addrs.size(); ++i) {
            if (i > 0) addrList += ",";
            addrList += std::to_string(addrs[i]);
        }

        std::string sql = "SELECT addr, value FROM " + tableName +
                         " a WHERE (addr, time) IN (SELECT addr, MAX(time) FROM " + tableName +
                         " GROUP BY addr) AND addr IN (" + addrList + ")";

        if (mysql_query(conn, sql.c_str())) {
            LOG_ACTION(std::string(tableName) + " selectMultipleData failed: " + std::string(mysql_error(conn)));
            throw std::runtime_error(std::string(tableName) + " selectMultipleData failed: " + std::string(mysql_error(conn)));
        }

        MYSQL_RES *result = mysql_store_result(conn);
        if (result == nullptr) {
            LOG_ACTION(std::string(tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
            throw std::runtime_error(std::string(tableName) + " mysql_store_result failed: " + std::string(mysql_error(conn)));
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (row[0] && row[1]) {
                int addr = std::atoi(row[0]);
                float value = std::atof(row[1]);
                resultMap[addr] = value;
            }
        }

        mysql_free_result(result);

        // 对于没有查询到的地址，设置默认值0
        for (int addr : addrs) {
            if (resultMap.find(addr) == resultMap.end()) {
                resultMap[addr] = 0.0f;
            }
        }

    } catch (const std::exception& e) {
        LOG_ACTION("selectMultipleData exception: " + std::string(e.what()));
        // 发生异常时，为所有地址设置默认值
        for (int addr : addrs) {
            resultMap[addr] = 0.0f;
        }
    }

    return resultMap;
}

void MySQLDatabase::checkAndCleanTable(const std::string& tableName, double maxSizeMB) {
    // 获取表大小（以MB为单位）
    std::string sizeSql = "SELECT ((DATA_LENGTH + INDEX_LENGTH) / 1024 / 1024) AS size_mb "
                         "FROM information_schema.TABLES "
                         "WHERE TABLE_SCHEMA = DATABASE() "
                         "AND TABLE_NAME = '" + tableName + "';";

    if (mysql_query(conn, sizeSql.c_str())) {
        throw std::runtime_error("Failed to get table size: " + std::string(mysql_error(conn)));
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        throw std::runtime_error("mysql_store_result failed: " + std::string(mysql_error(conn)));
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row || !row[0]) {
        mysql_free_result(result);
        throw std::runtime_error("Failed to get table size information");
    }

    double currentSize = std::stod(row[0]);
    mysql_free_result(result);

    // 如果表大小超过阈值，删除最旧的记录直到表大小小于阈值
    if (currentSize > maxSizeMB) {
        // 计算需要删除的记录比例
        double deleteRatio = (currentSize - maxSizeMB) / currentSize;

        // 获取总记录数
        std::string countSql = "SELECT COUNT(*) FROM " + tableName + ";";
        if (mysql_query(conn, countSql.c_str())) {
            throw std::runtime_error("Failed to get record count: " + std::string(mysql_error(conn)));
        }

        result = mysql_store_result(conn);
        if (!result) {
            throw std::runtime_error("mysql_store_result failed: " + std::string(mysql_error(conn)));
        }

        row = mysql_fetch_row(result);
        if (!row || !row[0]) {
            mysql_free_result(result);
            throw std::runtime_error("Failed to get record count");
        }

        int totalRecords = std::stoi(row[0]);
        mysql_free_result(result);

        // 计算需要删除的记录数
        int recordsToDelete = static_cast<int>(totalRecords * deleteRatio);

        // 删除最旧的记录
        std::string deleteSql = "DELETE FROM " + tableName +
                               " ORDER BY time ASC LIMIT " +
                               std::to_string(recordsToDelete) + ";";

        if (mysql_query(conn, deleteSql.c_str())) {
            throw std::runtime_error("Failed to delete old records: " + std::string(mysql_error(conn)));
        }
    }
}

void MySQLDatabase::copyTable(const std::string &sourceTableName, const std::string &targetTableName) {
    std::string sql = "UPDATE " + targetTableName + " JOIN " + sourceTableName + " ON " + targetTableName + ".addr = " +
            sourceTableName + ".addr SET " + targetTableName + ".value = " + sourceTableName + ".value";
    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(sourceTableName + " copy table failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error("copy table failed: " + std::string(mysql_error(conn)));
    }
}

// 将表中除第一行外的所有行的value列设置为0
void MySQLDatabase::resetAllValuesToZeroExceptFirst(const std::string& tableName) {
    // 使用子查询获取第一行的id，然后更新除第一行外的所有行
    std::string sql = "UPDATE " + tableName + " SET value = 0, time = NOW() "
                     "WHERE id NOT IN (SELECT * FROM (SELECT MIN(id) FROM " + tableName + ") AS temp)";

    if (mysql_query(conn, sql.c_str())) {
        LOG_ACTION(std::string(tableName) + " resetAllValuesToZeroExceptFirst failed: " + std::string(mysql_error(conn)));
        throw std::runtime_error(std::string(tableName) + " resetAllValuesToZeroExceptFirst failed: " + std::string(mysql_error(conn)));
    }
}
