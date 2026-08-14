#ifndef I_DATA_SINK_H
#define I_DATA_SINK_H

#include <map>
#include <string>
#include <utility>
#include <vector>

/**
 * 实时数据落库的最小读写接口。
 * ModbusPollEngine 及各采集模块只依赖此抽象，不再直接依赖 MySQLDB_1；
 * 生产环境用 MySQLDatabase（实现本接口），测试可用桩实现。
 */
class IDataSink {
public:
    virtual ~IDataSink() = default;

    /** 批量更新实时表：每点 (addr, value) 写最新值（一次 UPDATE 完成） */
    virtual void updateRealtime(const std::string& table,
                                const std::vector<std::pair<int, double>>& points) = 0;

    /** 单点更新（通讯状态等单值落库） */
    virtual void updateValue(int addr, double value, const std::string& table) = 0;

    /** 读整型值（最新一条） */
    virtual int readInt(int addr, const std::string& table) = 0;

    /** 读浮点值（最新一条） */
    virtual double readFloat(int addr, const std::string& table) = 0;

    /** 批量读多个地址的最新值 → map（缺省地址补 0） */
    virtual std::map<int, float> readBatch(const std::string& table,
                                           const std::vector<int>& addrs) = 0;
};

#endif  // I_DATA_SINK_H
