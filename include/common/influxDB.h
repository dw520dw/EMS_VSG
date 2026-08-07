#ifndef INFLUX_H
#define INFLUX_H

#include <iostream>
#include <thread>
#include <sstream>
#include <list>
#include <string>
#include <curl/curl.h>
#include "logger.h"


class influxDB{
public:
    void insert(const std::string& sql);
private:
    CURL *curl;
};

class DatabaseList {
public:
    struct modbusData{
        std::string EN;
        double value;
        modbusData(const std::string& en, double val) : EN(en), value(val) {};
    };
    void clearData();
    void addData(const std::string& EN, double value);
    std::string spliceData(const std::string& tableName);
private:
    std::list<modbusData>modbusDatas;
};


#endif
