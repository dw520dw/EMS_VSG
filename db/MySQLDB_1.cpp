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

/** 统一 DB 错误出口：LOG_ACTION + 抛异常，避免各调用点重复拼装 */
[[noreturn]] void throwDbError(const char* op, const std::string& table, const std::string& msg)
{
    const std::string text = std::string(op) + "(" + table + ") failed: " + msg;
    LOG_ACTION(text);
    throw std::runtime_error(text);
}

/** SQL 字符串字面量转义（WHERE TABLE_NAME = '...' 这类场景），防注入 */
std::string escapeSqlString(const std::string& s)
{
    std::string out;
    out.reserve(s.size() + 2);
    out.push_back('\'');
    for (char c : s)
    {
        if (c == '\'' || c == '\\')
        {
            out.push_back('\\');
        }
        out.push_back(c);
    }
    out.push_back('\'');
    return out;
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
        if (conn == nullptr) {
            throw std::runtime_error("Failed to init MySQL connection (OOM?)");
        }
        if (mysql_real_connect(conn, host, user, password, database, 3306, nullptr, 0) == nullptr) {
            const std::string err = mysql_error(conn);
            mysql_close(conn);
            throw std::runtime_error("Failed to create connection: " + err);
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

// ============ IDataSink 实现 ============

void MySQLDatabase::updateRealtime(const std::string& table,
                                   const std::vector<std::pair<int, double>>& points) {
    if (points.empty()) {
        return;
    }
    databaseList list;
    for (const auto& p : points) {
        list.addData(p.first, p.second);
    }
    const std::string sql = list.spliceData(table);
    if (!sql.empty()) {
        insert(sql);
    }
}

void MySQLDatabase::updateValue(int addr, double value, const std::string& table) {
    update(addr, value, table);
}

int MySQLDatabase::readInt(int addr, const std::string& table) {
    return select(addr, table);
}

double MySQLDatabase::readFloat(int addr, const std::string& table) {
    return select_float(addr, table);
}

std::map<int, float> MySQLDatabase::readBatch(const std::string& table,
                                              const std::vector<int>& addrs) {
    return selectMultipleData(table, addrs);
}

// 创建表
void MySQLDatabase::createTable(const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    std::string sql = "CREATE TABLE IF NOT EXISTS " + t + " ("
                                                          "id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
                                                          "addr INT NOT NULL,"
                                                          "CN VARCHAR(100) NOT NULL,"
                                                          "EN VARCHAR(100) NOT NULL,"
                                                          "value FLOAT NOT NULL,"
                                                          "time TIMESTAMP NOT NULL);";
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("createTable", tableName, mysql_error(conn));
    }
}

// 一次性插入多个数据
void MySQLDatabase::insert(const std::string& sql) {
    if (isSkippableInsertSql(sql)) {
        return;
    }
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("insert", "", mysql_error(conn));
    }
}

// 更新表里数据（预处理语句：值与主键作参数）
void MySQLDatabase::update(int addr, int newValue, const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "UPDATE " + t + " SET value=?, time=NOW() WHERE addr=?";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) {
        throwDbError("update", tableName, mysql_error(conn));
    }
    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    int a = addr;
    MYSQL_BIND param[2]{};
    param[0].buffer_type = MYSQL_TYPE_LONG;
    param[0].buffer = &newValue;
    param[0].is_null = 0;
    param[1].buffer_type = MYSQL_TYPE_LONG;
    param[1].buffer = &a;
    param[1].is_null = 0;
    if (mysql_stmt_bind_param(stmt, param) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    if (mysql_stmt_execute(stmt) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    mysql_stmt_close(stmt);
}

// 更新表里数据，重载函数（预处理语句）
void MySQLDatabase::update(int addr, double newValue, const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "UPDATE " + t + " SET value=?, time=NOW() WHERE addr=?";
    MYSQL_STMT* stmt = mysql_stmt_init(conn);
    if (!stmt) {
        throwDbError("update", tableName, mysql_error(conn));
    }
    if (mysql_stmt_prepare(stmt, sql.c_str(), sql.size()) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    int a = addr;
    MYSQL_BIND param[2]{};
    param[0].buffer_type = MYSQL_TYPE_DOUBLE;
    param[0].buffer = &newValue;
    param[0].is_null = 0;
    param[1].buffer_type = MYSQL_TYPE_LONG;
    param[1].buffer = &a;
    param[1].is_null = 0;
    if (mysql_stmt_bind_param(stmt, param) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    if (mysql_stmt_execute(stmt) != 0) {
        const std::string e = mysql_stmt_error(stmt);
        mysql_stmt_close(stmt);
        throwDbError("update", tableName, e);
    }
    mysql_stmt_close(stmt);
}

// 查询表里最新插入的 value 的值（取第一行第一列，NULL 安全）
int MySQLDatabase::select(int addr, const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "SELECT value FROM " + t + " a WHERE (addr, time) IN "
                            "(SELECT addr, MAX(time) FROM " + t + " GROUP BY addr) AND addr = " + std::to_string(addr);
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("select", tableName, mysql_error(conn));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        throwDbError("select", tableName, mysql_error(conn));
    }
    MYSQL_ROW row = mysql_fetch_row(result);
    int value = 0;
    if (row && row[0] && row[0][0] != '\0') {
        value = std::atoi(row[0]);
    }
    mysql_free_result(result);
    return value;
}

// 查询浮点数值（取第一行第一列，NULL 安全）
double MySQLDatabase::select_float(int addr, const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "SELECT value FROM " + t + " a WHERE (addr, time) IN "
                            "(SELECT addr, MAX(time) FROM " + t + " GROUP BY addr) AND addr = " + std::to_string(addr);
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("select_float", tableName, mysql_error(conn));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        throwDbError("select_float", tableName, mysql_error(conn));
    }
    MYSQL_ROW row = mysql_fetch_row(result);
    double value = 0.0;
    if (row && row[0] && row[0][0] != '\0') {
        value = std::atof(row[0]);
    }
    mysql_free_result(result);
    return value;
}

// 删除其余数据，只保留通讯状态
void MySQLDatabase::deleteOldRows(const std::string& sourceTableName, const int count) {
    const std::string t = mysqlEscapeIdent(sourceTableName);
    // 删除所有数据的SQL语句
    const std::string sql = "DELETE FROM " + t + " WHERE id NOT IN "
                            "(SELECT id FROM (SELECT id FROM " + t + " ORDER BY id DESC LIMIT " + std::to_string(count) + " ) AS temp)";
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("deleteOldRows", sourceTableName, mysql_error(conn));
    }
}

std::map<int, float> MySQLDatabase::selectAllData(const std::string& tableName, int count){
    std::map<int, float> myMap;
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "SELECT addr, value FROM " + t + " ORDER BY id DESC LIMIT " + std::to_string(count);
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("selectAllData", tableName, mysql_error(conn));
    }
    MYSQL_RES *result = mysql_store_result(conn);
    if (result == nullptr) {
        throwDbError("selectAllData", tableName, mysql_error(conn));
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result))) {
        if (!row[0] || !row[1]) {
            continue;
        }
        myMap[std::atoi(row[0])] = std::atof(row[1]);
    }
    mysql_free_result(result);
    return myMap;
}

std::vector<std::pair<std::string, double>> MySQLDatabase::selectAllEnValues(
    const std::string& tableName)
{
    std::vector<std::pair<std::string, double>> out;
    const std::string t = mysqlEscapeIdent(tableName);
    const std::string sql = "SELECT EN, value FROM " + t + " ORDER BY addr";
    if (mysql_query(conn, sql.c_str()))
    {
        throwDbError("selectAllEnValues", tableName, mysql_error(conn));
    }
    MYSQL_RES* result = mysql_store_result(conn);
    if (result == nullptr)
    {
        throwDbError("selectAllEnValues", tableName, mysql_error(conn));
    }
    MYSQL_ROW row;
    while ((row = mysql_fetch_row(result)))
    {
        if (!row[0] || !row[1])
        {
            continue;
        }
        const std::string en = row[0];
        if (en.empty())
        {
            continue;
        }
        out.emplace_back(en, std::atof(row[1]));
    }
    mysql_free_result(result);
    return out;
}

// 批量查询指定地址的数据
std::map<int, float> MySQLDatabase::selectMultipleData(const std::string& tableName, const std::vector<int>& addrs) {
    std::map<int, float> resultMap;

    if (addrs.empty()) {
        return resultMap;
    }

    try {
        // 构建 IN 查询语句（addr 为 int，std::to_string 生成，无注入面）
        std::string addrList = "";
        for (size_t i = 0; i < addrs.size(); ++i) {
            if (i > 0) addrList += ",";
            addrList += std::to_string(addrs[i]);
        }

        const std::string t = mysqlEscapeIdent(tableName);
        const std::string sql = "SELECT addr, value FROM " + t +
                         " a WHERE (addr, time) IN (SELECT addr, MAX(time) FROM " + t +
                         " GROUP BY addr) AND addr IN (" + addrList + ")";

        if (mysql_query(conn, sql.c_str())) {
            throwDbError("selectMultipleData", tableName, mysql_error(conn));
        }

        MYSQL_RES *result = mysql_store_result(conn);
        if (result == nullptr) {
            throwDbError("selectMultipleData", tableName, mysql_error(conn));
        }

        MYSQL_ROW row;
        while ((row = mysql_fetch_row(result))) {
            if (row[0] && row[1]) {
                resultMap[std::atoi(row[0])] = std::atof(row[1]);
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
    const std::string t = mysqlEscapeIdent(tableName);
    // 获取表大小（以MB为单位）；TABLE_NAME 是字符串字面量，走 escapeSqlString 防注入
    const std::string sizeSql = "SELECT ((DATA_LENGTH + INDEX_LENGTH) / 1024 / 1024) AS size_mb "
                         "FROM information_schema.TABLES "
                         "WHERE TABLE_SCHEMA = DATABASE() "
                         "AND TABLE_NAME = " + escapeSqlString(tableName);

    if (mysql_query(conn, sizeSql.c_str())) {
        throwDbError("checkAndCleanTable", tableName, mysql_error(conn));
    }

    MYSQL_RES *result = mysql_store_result(conn);
    if (!result) {
        throwDbError("checkAndCleanTable", tableName, mysql_error(conn));
    }

    MYSQL_ROW row = mysql_fetch_row(result);
    if (!row || !row[0]) {
        mysql_free_result(result);
        throwDbError("checkAndCleanTable", tableName, "failed to get table size information");
    }

    double currentSize = std::stod(row[0]);
    mysql_free_result(result);

    // 如果表大小超过阈值，删除最旧的记录直到表大小小于阈值
    if (currentSize > maxSizeMB) {
        // 计算需要删除的记录比例
        double deleteRatio = (currentSize - maxSizeMB) / currentSize;

        // 获取总记录数
        const std::string countSql = "SELECT COUNT(*) FROM " + t;
        if (mysql_query(conn, countSql.c_str())) {
            throwDbError("checkAndCleanTable", tableName, mysql_error(conn));
        }

        result = mysql_store_result(conn);
        if (!result) {
            throwDbError("checkAndCleanTable", tableName, mysql_error(conn));
        }

        row = mysql_fetch_row(result);
        if (!row || !row[0]) {
            mysql_free_result(result);
            throwDbError("checkAndCleanTable", tableName, "failed to get record count");
        }

        int totalRecords = std::stoi(row[0]);
        mysql_free_result(result);

        // 计算需要删除的记录数
        int recordsToDelete = static_cast<int>(totalRecords * deleteRatio);

        // 删除最旧的记录
        const std::string deleteSql = "DELETE FROM " + t +
                               " ORDER BY time ASC LIMIT " +
                               std::to_string(recordsToDelete);

        if (mysql_query(conn, deleteSql.c_str())) {
            throwDbError("checkAndCleanTable", tableName, mysql_error(conn));
        }
    }
}

void MySQLDatabase::copyTable(const std::string &sourceTableName, const std::string &targetTableName) {
    const std::string s = mysqlEscapeIdent(sourceTableName);
    const std::string t = mysqlEscapeIdent(targetTableName);
    const std::string sql = "UPDATE " + t + " JOIN " + s + " ON " + t + ".addr = " +
            s + ".addr SET " + t + ".value = " + s + ".value";
    if (mysql_query(conn, sql.c_str())) {
        throwDbError("copyTable", sourceTableName + "->" + targetTableName, mysql_error(conn));
    }
}

// 将表中除第一行外的所有行的value列设置为0
void MySQLDatabase::resetAllValuesToZeroExceptFirst(const std::string& tableName) {
    const std::string t = mysqlEscapeIdent(tableName);
    // 使用子查询获取第一行的id，然后更新除第一行外的所有行
    const std::string sql = "UPDATE " + t + " SET value = 0, time = NOW() "
                     "WHERE id NOT IN (SELECT * FROM (SELECT MIN(id) FROM " + t + ") AS temp)";

    if (mysql_query(conn, sql.c_str())) {
        throwDbError("resetAllValuesToZeroExceptFirst", tableName, mysql_error(conn));
    }
}
