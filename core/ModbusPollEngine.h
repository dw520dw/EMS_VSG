#ifndef MODBUS_POLL_ENGINE_H
#define MODBUS_POLL_ENGINE_H

#include "IDataSink.h"
#include "IModbusBus.h"
#include "ModbusDeviceProfile.h"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * 配置驱动的 Modbus 采集引擎（只写实时库，经 IDataSink 抽象落库）。
 *
 * 读层：Telegraf 式 fields（fc/address/type/scale），自动合并连续地址读请求。
 * 写库层：sink.sequential 或 explicit；引擎不依赖具体存储实现。
 */
class ModbusPollEngine {
public:
    using PostDecodeHook = std::function<void(ModbusPollEngine& engine, IDataSink& sink)>;
    /** 每轮轮询创建数据接收器（如 MySQLDatabase 借用连接池连接） */
    using SinkFactory = std::function<std::unique_ptr<IDataSink>()>;

    ModbusPollEngine(IModbusBus& bus, ModbusDeviceProfile profile);

    const ModbusDeviceProfile& profile() const { return profile_; }
    ModbusDeviceProfile& profile() { return profile_; }
    IModbusBus& bus() { return bus_; }
    double pt() const { return pt_; }
    double ct() const { return ct_; }
    int commFlag() const { return commFlag_; }

    void setPostDecodeHook(PostDecodeHook hook) { postDecodeHook_ = std::move(hook); }
    void setTableSuffix(int suffix) { profile_.table_suffix = suffix; }

    /** 多簇：address = base + (clusterIndex-1)*cluster_stride（簇号从 1 起） */
    void setClusterIndex(int clusterIndex);

    void setSkipProbe(bool skip) { skipProbe_ = skip; }
    void setInheritCommFlag(int flag) { inheritCommFlag_ = flag; }
    /** 多从站复用同一引擎时，切台前清寄存器缓存，避免把上一台残留值写入本表 */
    void clearCaches()
    {
        regCache_.clear();
        bitCache_.clear();
    }

    void setValue(const std::string& name, double value) { values_[name] = value; }
    double getValue(const std::string& name, double def = 0.0) const;

    bool pollOnce(IDataSink& sink);

    void runLoop(SinkFactory factory);
    void runLoopFixedPeriod(SinkFactory factory);

private:
    struct ReadRequest {
        ModbusFc fc = ModbusFc::HoldingRegisters;
        int start = 0;
        int count = 0;
        bool isBit = false;
        std::vector<uint16_t> regBuf;
        std::vector<uint8_t> bitBuf;
    };

    bool checkEnabled(IDataSink& sink);
    bool checkComm(IDataSink& sink);
    void syncPtCt(IDataSink& sink);
    void rebuildReadPlan();
    bool readAllFields();
    void decodePoints();
    void writeRealtime(IDataSink& sink);
    void frameGap() const;

    double decodeRaw(const ModbusPointDef& pt) const;
    bool lookupReg(ModbusFc fc, int addr, uint16_t& out) const;
    bool lookupBit(ModbusFc fc, int addr, uint8_t& out) const;

    static int registerSpan(ModbusPointType type);

    IModbusBus& bus_;
    ModbusDeviceProfile profile_;
    PostDecodeHook postDecodeHook_;

    /** 各 field 的第 1 簇基址，供 setClusterIndex 使用 */
    std::vector<int> fieldAddressBases_;
    std::vector<ReadRequest> readPlan_;

    bool skipProbe_ = false;
    int inheritCommFlag_ = 0;

    double pt_ = 1.0;
    double ct_ = 1.0;
    int commFlag_ = 0;
    int commErrCount_ = 0;
    uint16_t probeBuf_[8] = {0};

    /** key = (fc<<28) ^ address */
    std::unordered_map<uint32_t, uint16_t> regCache_;
    std::unordered_map<uint32_t, uint8_t> bitCache_;
    std::unordered_map<std::string, double> values_;
};

#endif  // MODBUS_POLL_ENGINE_H
