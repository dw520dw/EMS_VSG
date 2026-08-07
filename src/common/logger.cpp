#include "logger.h"

void Logger::rotateLogFile() {
    if (!std::filesystem::exists(logFileName_)) {
        std::cerr << "日志文件不存在: " << logFileName_ << std::endl;
        return;
    }

    // std::cout << "当前工作目录: " << std::filesystem::current_path() << std::endl;
    std::string newPath = R"(/userdata/iEMS-MG1000/collectLog/)";    
    std::filesystem::current_path(newPath);
    std::time_t now = std::time(nullptr);
    std::tm *localTime = std::localtime(&now);

    std::ostringstream oss;
    oss << "collect_" << std::put_time(localTime, "%Y%m%d_%H%M%S") << ".log";
    std::string newFilename = oss.str();

    try {
        std::filesystem::rename(logFileName_, newFilename);
        std::cout << "日志文件已重命名为: " << newFilename << std::endl;

        std::vector<std::filesystem::path> logFiles;
        for (const auto &entry : std::filesystem::directory_iterator(".")) {
            if (entry.path().filename().string().find("collect_") == 0 && entry.path().extension() == ".log") {
                logFiles.push_back(entry.path());
            }
        }

        if (logFiles.size() > maxLogs_) {
            std::sort(logFiles.begin(), logFiles.end(),
                      [](const std::filesystem::path &a, const std::filesystem::path &b) {
                          return std::filesystem::last_write_time(a) < std::filesystem::last_write_time(b);
                      });
            std::filesystem::remove(logFiles.front());
            std::cout << "删除最旧的日志文件: " << logFiles.front() << std::endl;
        }

    } catch (const std::filesystem::filesystem_error& e) {
        std::cerr << "重命名日志文件时出错: " << e.what() << std::endl;
    }
}

void Logger::logAction(const std::string &action, const char *file, int line) {
    std::time_t now_c = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    char buffer[100];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", std::localtime(&now_c));

    std::ofstream logFile(logFileName_, std::ios_base::app);
    if (logFile.is_open()) {
        logFile << buffer << "  -  " << file << "  -  " << line << "  ->  " << action << std::endl;
    }

    if (logFile.tellp() > maxLogSize_) { // 超过最大大小时旋转日志
        logFile.close();
        rotateLogFile();
    }
}
