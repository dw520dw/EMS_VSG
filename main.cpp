#include "Agc.h"
#include "AMC_Meter.h"
#include "Bamu.h"
#include "Config.h"
#include "Dido.h"
#include "MySQLDB_1.h"
#include "PCS_Smarten.h"
#include "SunPv.h"
#include "crash_handler.h"
#include "logger.h"
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

namespace {

std::string readRss()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            return line;
        }
    }
    return "VmRSS: N/A";
}

}  // namespace

int main(int argc, char* argv[])
{
    // 崩溃诊断必须最先安装：SIGPIPE 忽略 + 崩溃信号/未捕获异常记录到 collect.log
    installCrashHandler();

    const char* configPath = Config::MODBUS_DEVICES_CONFIG;
    if (argc >= 2 && argv[1] != nullptr && argv[1][0] != '\0') {
        configPath = argv[1];
    }

    std::cout << "等待系统就绪 10s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    LOG_ACTION("系统就绪 10s...");

    MySQLConnectionPool pool(Config::DB_HOST, Config::DB_USER, Config::DB_PASSWORD,
                             Config::DB_NAME, Config::DB_POOL_SIZE);
    LOG_ACTION("实时数据库连接池创建完成");

    auto amcLoadDg = std::make_shared<AMC_Meter>(Config::LOAD_METER_DEVICE_ID,
                                                 Config::DG_METER_DEVICE_ID, configPath);
    auto amcPvEss = std::make_shared<AMC_Meter>(Config::PV_METER_DEVICE_ID,
                                                Config::ESS_METER_DEVICE_ID, configPath);
    auto dido = std::make_shared<Dido>(configPath);
    auto pcs = std::make_shared<PCS_Smarten>(configPath);
    auto bamu = std::make_shared<Bamu>(configPath);
    auto sunPv = std::make_shared<SunPv>(configPath);
    auto agc = std::make_shared<Agc>(configPath);

    std::thread t1(&AMC_Meter::runPairThread, amcLoadDg.get(), std::ref(pool));
    std::thread t2(&AMC_Meter::runPairThread, amcPvEss.get(), std::ref(pool));
    std::thread t3(&Dido::didoThread, dido.get(), std::ref(pool));
    std::thread t4(&PCS_Smarten::runPcsThread, pcs.get(), std::ref(pool));
    std::thread t5(&Bamu::runBamuDataThread, bamu.get(), std::ref(pool));
    std::thread t6(&SunPv::runSunPvThread, sunPv.get(), std::ref(pool));
    std::thread t7(&Agc::runAgcThread, agc.get(), std::ref(pool));
    t1.detach();
    t2.detach();
    t3.detach();
    t4.detach();
    t5.detach();
    t6.detach();
    t7.detach();

    std::cout << "所有采集线程已启动（无 Influx），config=" << configPath << std::endl;
    // 主循环：每 10 分钟把进程 RSS 打进 collect.log。
    // 若 RSS 随时间单调上升并逼近内存上限，即可确认“泄漏 → OOM 被杀”。
    while (true) {
        std::this_thread::sleep_for(std::chrono::minutes(10));
        LOG_ACTION("周期内存监控 " + readRss());
    }
    return 0;
}
