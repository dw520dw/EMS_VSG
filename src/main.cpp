#include "PCS_Smarten.h"
#include "Bamu.h"
#include "Dido.h"
#include <iostream>
#include <thread>
#include <memory>
#include <chrono>
#include <cstdlib>
#include <curl/curl.h>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <csignal>
#include <fstream>
#include <string>
#include "MySQLDB_1.h"
#include "AMC_Meter.h"
#include "Agc.h"
#include "SunPv.h"
#include "Config.h"

int main() {
    std::cout << "等待系统就绪 10s..." << std::endl;
    std::this_thread::sleep_for(std::chrono::seconds(10));
    LOG_ACTION("系统就绪 10s...");

     //初始化历史数据库curl
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {

        std::cerr << "curl_global_init failed" << std::endl;

        return 1;

    }
    //清理curl
    std::atexit([]() { curl_global_cleanup(); });
    LOG_ACTION("历史数据库初始化完成");
    // 创建连接池
    MySQLConnectionPool pool(
        Config::DB_HOST,
        Config::DB_USER,
        Config::DB_PASSWORD,
        Config::DB_NAME,
        Config::DB_POOL_SIZE
    );
    MySQLDatabase db(pool);//创建数据库实例
    LOG_ACTION("实时数据库实例创建完成");
     // 光伏逆变器（Modbus RTU）
     /*std::shared_ptr<SunPv> sunPv = std::make_shared<SunPv>(
        Config::PORT0,
        Config::BAUD_RATE_NORMAL
    );
    LOG_ACTION("光伏逆变器实例创建完成");

    //柴发控制器
    std::shared_ptr<Agc> agc = std::make_shared<Agc>(
        Config::PORT1,
        Config::BAUD_RATE_NORMAL
    );*/
    LOG_ACTION("柴发电器实例创建完成");
    //负载电表，柴发电表实例
    std::shared_ptr<AMC_Meter> amcMeter1 = std::make_shared<AMC_Meter>(
        Config::PORT2,
        Config::BAUD_RATE_NORMAL
    );
    LOG_ACTION("负载电表实例创建完成");
     //光伏电表， ESS电表实例
    std::shared_ptr<AMC_Meter> amcMeter2 = std::make_shared<AMC_Meter>(
        Config::PORT3,
        Config::BAUD_RATE_NORMAL
    );
    LOG_ACTION("光伏电表， ESS电表实例创建完成");
   //DIDO实例（Modbus TCP）
    std::shared_ptr<Dido> dido = std::make_shared<Dido>(
        Config::DIDO_IP,
        Config::DIDO_PORT,
        Config::ADDRESS_1
    );
    LOG_ACTION("DIDO实例创建完成");
    // PCS（Modbus TCP）
    std::shared_ptr<PCS_Smarten> pcs = std::make_shared<PCS_Smarten>(
        Config::PCS_IP,
        Config::PCS_PORT,
        Config::ADDRESS_1
    );
    LOG_ACTION("PCS实例创建完成");
    // 创建BAMU实例(Modbus地址1)
    std::shared_ptr<Bamu> bamu = std::make_shared<Bamu>(
        Config::BAMU_BMS_IP,
        Config::BAMU_BMS_PORT,
        Config::ADDRESS_1
    );
    LOG_ACTION("BAMU和BMS实例创建完成");
    std::cout << "BAMU和BMS实例创建成功" << std::endl;
    std::thread bamu_data_thread(&Bamu::runBamuDataThread, bamu.get(), std::ref(pool));
    bamu_data_thread.detach();

    
    //std::thread agc_thread(&Agc::runAgcThread, agc.get(), std::ref(pool));
    std::thread amc_load_dg_thread(&AMC_Meter::runLoadDgThread, amcMeter1.get(), std::ref(pool));
    std::thread amc_pv_ess_thread(&AMC_Meter::runPvEssThread, amcMeter2.get(), std::ref(pool));
    //std::thread sunpv_thread(&SunPv::runSunPvThread, sunPv.get(), std::ref(pool));
    std::thread dido_thread(&Dido::didoThread, dido.get(), std::ref(pool));
    std::thread pcs_thread(&PCS_Smarten::runPcsThread, pcs.get(), std::ref(pool));
    
    //agc_thread.detach();
    amc_load_dg_thread.detach();
    amc_pv_ess_thread.detach();
    //sunpv_thread.detach();
    dido_thread.detach();
    pcs_thread.detach();

    std::cout << "所有线程已启动，程序将持续运行..." << std::endl;

    while (true) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }

    return 0;
}
