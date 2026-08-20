/**
 * 配置驱动采集引擎：Telegraf 式绝对地址读 + 独立写库映射。
 */

#include "ModbusPollEngine.h"
#include "ThreadManager.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <thread>

namespace {

constexpr int kMaxRegCount = 125;
constexpr int kMaxBitCount = 2000;

uint32_t cacheKey(ModbusFc fc, int addr)
{
    return (static_cast<uint32_t>(static_cast<int>(fc)) << 28) ^
           static_cast<uint32_t>(addr & 0x0FFFFFFF);
}

}  // namespace

int ModbusPollEngine::registerSpan(ModbusPointType type)
{
    switch (type)
    {
    case ModbusPointType::U32_HI_LO:
    case ModbusPointType::I32_HI_LO:
    case ModbusPointType::U32_LO_HI:
    case ModbusPointType::I32_LO_HI:
        return 2;
    default:
        return 1;
    }
}

ModbusPollEngine::ModbusPollEngine(IModbusBus& bus, ModbusDeviceProfile profile)
    : bus_(bus),
      profile_(std::move(profile)),
      pt_(profile_.default_pt),
      ct_(profile_.default_ct)
{
    fieldAddressBases_.reserve(profile_.points.size());
    for (const auto& pt : profile_.points)
    {
        fieldAddressBases_.push_back(pt.address);
    }
    rebuildReadPlan();
}

void ModbusPollEngine::setClusterIndex(int clusterIndex)
{
    const int idx = clusterIndex < 1 ? 1 : clusterIndex;
    for (size_t i = 0; i < profile_.points.size(); ++i)
    {
        auto& pt = profile_.points[i];
        if (pt.type == ModbusPointType::Virtual || pt.type == ModbusPointType::VirtualComm)
        {
            continue;
        }
        const int base = (i < fieldAddressBases_.size()) ? fieldAddressBases_[i] : pt.address;
        pt.address = base + (idx - 1) * pt.cluster_stride;
    }
    rebuildReadPlan();
}

void ModbusPollEngine::rebuildReadPlan()
{
    struct Span {
        ModbusFc fc;
        bool isBit;
        int start;
        int end;  // exclusive
    };
    std::vector<Span> spans;
    spans.reserve(profile_.points.size());

    for (const auto& pt : profile_.points)
    {
        if (pt.type == ModbusPointType::Virtual || pt.type == ModbusPointType::VirtualComm)
        {
            continue;
        }
        Span s;
        s.fc = pt.fc;
        if (pt.type == ModbusPointType::Bit)
        {
            s.isBit = true;
            s.start = pt.address;
            s.end = pt.address + 1;
        }
        else
        {
            s.isBit = false;
            s.start = pt.address;
            s.end = pt.address + registerSpan(pt.type);
        }
        spans.push_back(s);
    }

    std::sort(spans.begin(), spans.end(), [](const Span& a, const Span& b) {
        if (a.fc != b.fc)
            return static_cast<int>(a.fc) < static_cast<int>(b.fc);
        if (a.isBit != b.isBit)
            return !a.isBit && b.isBit;
        return a.start < b.start;
    });

    readPlan_.clear();
    for (const auto& s : spans)
    {
        const int maxCount = s.isBit ? kMaxBitCount : kMaxRegCount;
        if (!readPlan_.empty())
        {
            auto& last = readPlan_.back();
            if (last.fc == s.fc && last.isBit == s.isBit && s.start <= last.start + last.count &&
                (s.end - last.start) <= maxCount)
            {
                const int newEnd = std::max(last.start + last.count, s.end);
                last.count = newEnd - last.start;
                continue;
            }
        }
        ReadRequest req;
        req.fc = s.fc;
        req.isBit = s.isBit;
        req.start = s.start;
        req.count = s.end - s.start;
        if (req.count > maxCount)
        {
            req.count = maxCount;
        }
        readPlan_.push_back(std::move(req));
    }

    size_t regSlots = 0;
    size_t bitSlots = 0;
    for (auto& req : readPlan_)
    {
        if (req.isBit)
        {
            req.bitBuf.assign(static_cast<size_t>(req.count), 0);
            req.regBuf.clear();
            bitSlots += static_cast<size_t>(req.count);
        }
        else
        {
            req.regBuf.assign(static_cast<size_t>(req.count), 0);
            req.bitBuf.clear();
            regSlots += static_cast<size_t>(req.count);
        }
    }
    // 预留容量，减少轮询时 unordered_map 反复 rehash
    regCache_.reserve(regSlots);
    bitCache_.reserve(bitSlots);
}

double ModbusPollEngine::getValue(const std::string& name, double def) const
{
    const auto it = values_.find(name);
    return it != values_.end() ? it->second : def;
}

void ModbusPollEngine::frameGap() const
{
    if (profile_.inter_frame_ms > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(profile_.inter_frame_ms));
    }
}

bool ModbusPollEngine::checkEnabled(IDataSink& sink)
{
    if (!profile_.enable.use_qt || profile_.enable.qt_addr < 0)
    {
        return true;
    }
    return sink.readInt(profile_.enable.qt_addr, "qt") == profile_.enable.enabled_value;
}

bool ModbusPollEngine::checkComm(IDataSink& sink)
{
    const std::string table = profile_.resolvedMysqlTable();

    // 离线时立刻写 Online/com_alarm；在线时 Online 由 writeRealtime 的 virtual_comm 落库，
    // 这里只清 com_alarm（若配置了）。
    auto markOffline = [&]() {
        sink.updateValue(profile_.comm.mysql_online_addr, 1.0, table);
        if (profile_.comm.com_alarm_addr >= 0)
        {
            sink.updateValue(profile_.comm.com_alarm_addr, 1.0, profile_.comm.com_alarm_table);
        }
    };
    auto clearComAlarm = [&]() {
        if (profile_.comm.com_alarm_addr >= 0)
        {
            sink.updateValue(profile_.comm.com_alarm_addr, 0.0, profile_.comm.com_alarm_table);
        }
    };

    if (skipProbe_)
    {
        commFlag_ = inheritCommFlag_;
        if (commFlag_ != 0)
        {
            markOffline();
            return false;
        }
        clearComAlarm();
        return true;
    }

    if (!profile_.comm.enabled)
    {
        commFlag_ = 0;
        return true;
    }

    bus_.setSlave(profile_.slave);
    const bool ok =
        bus_.probe(profile_.comm.probe_addr, profile_.comm.probe_count, probeBuf_, table);
    frameGap();

    if (!ok)
    {
        ++commErrCount_;
        if (commErrCount_ > profile_.comm.fail_threshold)
        {
            commFlag_ = 1;
            markOffline();
        }
        return false;
    }

    commErrCount_ = 0;
    commFlag_ = 0;
    clearComAlarm();
    return true;
}

void ModbusPollEngine::syncPtCt(IDataSink& sink)
{
    if (!profile_.pt_ct_sync.enabled)
    {
        pt_ = profile_.default_pt;
        ct_ = profile_.default_ct;
        return;
    }

    const auto& s = profile_.pt_ct_sync;
    uint16_t devicePt = 0;
    uint16_t deviceCt = 0;
    if (s.read_reg >= 0)
    {
        uint16_t buf[2] = {0, 0};
        if (bus_.readRegisters(s.read_reg, 2, buf) != -1)
        {
            devicePt = buf[0];
            deviceCt = buf[1];
        }
        frameGap();
    }

    if (s.pt_qt_addr >= 0)
    {
        const int qtPt = static_cast<int>(sink.readFloat(s.pt_qt_addr, "qt"));
        if (qtPt != static_cast<int>(devicePt) && qtPt >= s.min_value && qtPt <= s.max_value &&
            s.pt_reg >= 0)
        {
            bus_.writeRegister(s.pt_reg, static_cast<uint16_t>(qtPt), profile_.resolvedMysqlTable());
            frameGap();
        }
        pt_ = static_cast<double>(qtPt);
    }
    else if (s.read_reg >= 0 && devicePt > 0)
    {
        // 电表等：直接用设备寄存器中的 PT
        pt_ = static_cast<double>(devicePt);
    }
    else
    {
        pt_ = profile_.default_pt;
    }

    if (s.ct_qt_addr >= 0)
    {
        const int qtCt = static_cast<int>(sink.readFloat(s.ct_qt_addr, "qt"));
        if (qtCt != static_cast<int>(deviceCt) && qtCt >= s.min_value && qtCt <= s.max_value &&
            s.ct_reg >= 0)
        {
            bus_.writeRegister(s.ct_reg, static_cast<uint16_t>(qtCt), profile_.resolvedMysqlTable());
            frameGap();
        }
        ct_ = static_cast<double>(qtCt);
    }
    else if (s.read_reg >= 0 && deviceCt > 0)
    {
        ct_ = static_cast<double>(deviceCt);
    }
    else
    {
        ct_ = profile_.default_ct;
    }
}

bool ModbusPollEngine::readAllFields()
{
    // 不整表 clear：某段读失败时保留上轮成功值，避免把库写成 0
    bool anyOk = false;
    bool anyFail = false;

    for (auto& req : readPlan_)
    {
        bool ok = false;
        if (req.isBit)
        {
            if (static_cast<int>(req.bitBuf.size()) < req.count)
            {
                req.bitBuf.resize(static_cast<size_t>(req.count));
            }
            ok = bus_.readBlock(req.fc, req.start, req.count, nullptr, req.bitBuf.data()) != -1;
            if (ok)
            {
                for (int i = 0; i < req.count; ++i)
                {
                    bitCache_[cacheKey(req.fc, req.start + i)] = req.bitBuf[static_cast<size_t>(i)];
                }
            }
        }
        else
        {
            if (static_cast<int>(req.regBuf.size()) < req.count)
            {
                req.regBuf.resize(static_cast<size_t>(req.count));
            }
            ok = bus_.readBlock(req.fc, req.start, req.count, req.regBuf.data(), nullptr) != -1;
            if (ok)
            {
                for (int i = 0; i < req.count; ++i)
                {
                    regCache_[cacheKey(req.fc, req.start + i)] = req.regBuf[static_cast<size_t>(i)];
                }
            }
        }

        if (ok)
        {
            anyOk = true;
        }
        else
        {
            anyFail = true;
            std::cerr << "[" << profile_.id << "] read fail fc=" << static_cast<int>(req.fc)
                      << " start=" << req.start << " count=" << req.count << std::endl;
        }
        frameGap();
    }

    if (anyFail && !anyOk)
    {
        std::cerr << "[" << profile_.id << "] all read requests failed this cycle" << std::endl;
    }
    return anyOk;
}

bool ModbusPollEngine::lookupReg(ModbusFc fc, int addr, uint16_t& out) const
{
    const auto it = regCache_.find(cacheKey(fc, addr));
    if (it == regCache_.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

bool ModbusPollEngine::lookupBit(ModbusFc fc, int addr, uint8_t& out) const
{
    const auto it = bitCache_.find(cacheKey(fc, addr));
    if (it == bitCache_.end())
    {
        return false;
    }
    out = it->second;
    return true;
}

double ModbusPollEngine::decodeRaw(const ModbusPointDef& pt) const
{
    if (pt.type == ModbusPointType::VirtualComm)
    {
        return static_cast<double>(commFlag_);
    }
    if (pt.type == ModbusPointType::Virtual)
    {
        return 0.0;
    }

    if (pt.type == ModbusPointType::Bit)
    {
        uint8_t b = 0;
        if (!lookupBit(pt.fc, pt.address, b))
        {
            return 0.0;
        }
        return b ? 1.0 : 0.0;
    }

    uint16_t r0 = 0;
    if (!lookupReg(pt.fc, pt.address, r0))
    {
        return 0.0;
    }

    switch (pt.type)
    {
    case ModbusPointType::U16:
        return static_cast<double>(r0);
    case ModbusPointType::I16:
        return static_cast<double>(static_cast<int16_t>(r0));
    case ModbusPointType::U32_HI_LO:
    {
        // address=高字，address+1=低字
        uint16_t r1 = 0;
        if (!lookupReg(pt.fc, pt.address + 1, r1))
        {
            return 0.0;
        }
        return static_cast<double>((static_cast<uint32_t>(r0) << 16) | r1);
    }
    case ModbusPointType::I32_HI_LO:
    {
        uint16_t r1 = 0;
        if (!lookupReg(pt.fc, pt.address + 1, r1))
        {
            return 0.0;
        }
        const uint32_t u = (static_cast<uint32_t>(r0) << 16) | r1;
        return static_cast<double>(static_cast<int32_t>(u));
    }
    case ModbusPointType::U32_LO_HI:
    {
        // address=低字，address+1=高字
        uint16_t r1 = 0;
        if (!lookupReg(pt.fc, pt.address + 1, r1))
        {
            return 0.0;
        }
        return static_cast<double>((static_cast<uint32_t>(r1) << 16) | r0);
    }
    case ModbusPointType::I32_LO_HI:
    {
        uint16_t r1 = 0;
        if (!lookupReg(pt.fc, pt.address + 1, r1))
        {
            return 0.0;
        }
        const uint32_t u = (static_cast<uint32_t>(r1) << 16) | r0;
        return static_cast<double>(static_cast<int32_t>(u));
    }
    case ModbusPointType::RegBit:
        if (pt.bit < 0 || pt.bit > 15)
        {
            return 0.0;
        }
        return ((r0 >> pt.bit) & 0x1) ? 1.0 : 0.0;
    default:
        return 0.0;
    }
}

void ModbusPollEngine::decodePoints()
{
    std::unordered_map<std::string, double> keptVirtual;
    for (const auto& pt : profile_.points)
    {
        if (pt.type == ModbusPointType::Virtual)
        {
            const auto it = values_.find(pt.name);
            if (it != values_.end())
            {
                keptVirtual[pt.name] = it->second;
            }
        }
    }

    values_.clear();
    for (const auto& pt : profile_.points)
    {
        if (pt.type == ModbusPointType::Virtual)
        {
            const auto it = keptVirtual.find(pt.name);
            values_[pt.name] = (it != keptVirtual.end()) ? it->second : 0.0;
            continue;
        }
        const double raw = decodeRaw(pt);
        const double ptMul = (pt.pt_exp > 0) ? std::pow(pt_, pt.pt_exp) : 1.0;
        const double ctMul = (pt.ct_exp > 0) ? std::pow(ct_, pt.ct_exp) : 1.0;
        values_[pt.name] = raw * pt.scale * ptMul * ctMul + pt.bias;
    }
}

void ModbusPollEngine::writeRealtime(IDataSink& sink)
{
    // 写库映射来自 config/modbus/sink/<template>.json 物化的 write_points（显式 {name,addr}）
    std::vector<std::pair<int, double>> points;
    points.reserve(profile_.write_points.size());
    for (const auto& wp : profile_.write_points)
    {
        const auto it = values_.find(wp.name);
        if (it != values_.end())
        {
            points.emplace_back(wp.addr, it->second);
        }
    }
    sink.updateRealtime(profile_.resolvedMysqlTable(), points);
}

bool ModbusPollEngine::pollOnce(IDataSink& sink)
{
    if (!checkEnabled(sink))
    {
        return false;
    }
    if (!checkComm(sink))
    {
        return false;
    }

    bus_.setSlave(profile_.slave);
    syncPtCt(sink);
    const bool readOk = readAllFields();
    decodePoints();
    if (postDecodeHook_)
    {
        postDecodeHook_(*this, sink);
    }
    // 即使本轮部分/全部读失败，仍写库：sticky cache + virtual_comm 保持 Online 语义
    writeRealtime(sink);
    return readOk;
}

void ModbusPollEngine::runLoop(SinkFactory factory)
{
    auto& mgr = ThreadManager::instance();
    while (mgr.isRunning())
    {
        try
        {
            std::unique_ptr<IDataSink> sink = factory();
            pollOnce(*sink);
        }
        catch (const std::exception& e)
        {
            std::cerr << "[" << profile_.id << "] poll error: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[" << profile_.id << "] poll unknown error" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(profile_.poll_ms));
    }
    std::cout << "[" << profile_.id << "] 线程退出" << std::endl;
}

void ModbusPollEngine::runLoopFixedPeriod(SinkFactory factory)
{
    auto& mgr = ThreadManager::instance();
    while (mgr.isRunning())
    {
        const auto loopStart = std::chrono::steady_clock::now();
        try
        {
            std::unique_ptr<IDataSink> sink = factory();
            pollOnce(*sink);
        }
        catch (const std::exception& e)
        {
            std::cerr << "[" << profile_.id << "] poll error: " << e.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[" << profile_.id << "] poll unknown error" << std::endl;
        }
        const auto elapsed = std::chrono::steady_clock::now() - loopStart;
        const auto remain = std::chrono::milliseconds(profile_.poll_ms) -
                            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
        if (remain.count() > 0)
        {
            std::this_thread::sleep_for(remain);
        }
    }
    std::cout << "[" << profile_.id << "] 线程退出" << std::endl;
}
