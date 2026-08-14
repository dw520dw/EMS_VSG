// Logger.h
#ifndef LOGGER_H
#define LOGGER_H

#define LOG_ACTION(action) do { \
    std::ostringstream oss; \
    oss << action; \
    Logger::getInstance().logAction(oss.str(), __FILE__, __LINE__); \
} while(0)

#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <filesystem>
#include <vector>
#include <ctime>
#include <chrono>
#include <string>
#include <algorithm>

class Logger {
public:
    static Logger& getInstance(const std::string &logFilePath = R"(/userdata/iEMS-MG1000/collectLog/collect.log)", size_t maxLogSize = 1024 * 10000, size_t maxLogs = 3) {
        static Logger instance(logFilePath, maxLogSize, maxLogs);
        return instance;
    }

    void logAction(const std::string &action, const char *file, int line);

private:
    Logger(const std::string &logFilePath, size_t maxLogSize, size_t maxLogs)
            : logFileName_(logFilePath), maxLogSize_(maxLogSize), maxLogs_(maxLogs) {}

    void rotateLogFile();

    std::string logFileName_;
    size_t maxLogSize_;
    size_t maxLogs_;
};

#endif // LOGGER_H

