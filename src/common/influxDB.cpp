#include "influxDB.h"

void influxDB::insert(const std::string &sql) {
    std::string url = "http://127.0.0.1:8086/write?db=ems";
    curl = curl_easy_init();
    if (curl) {
        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, sql.c_str());
        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::cerr << "curl_easy_perform() failed: " << curl_easy_strerror(res) << std::endl;
        }
        curl_easy_cleanup(curl);
    } else {
        std::cerr << "Failed to initialize CURL." << std::endl;
    }
}

void DatabaseList::clearData() {
    modbusDatas.clear();
}

void DatabaseList::addData(const std::string& EN, double value) {
    modbusDatas.emplace_back(EN, value);
}

std::string DatabaseList::spliceData(const std::string& tableName){
    std::string data = tableName + " ";
    int count = 0;
    for (const auto& modbusData : modbusDatas){
        data += modbusData.EN + "=" + std::to_string(modbusData.value) + ",";
        count = count + 1;
    }
    data.pop_back();
    return data;
}
