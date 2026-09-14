//
// Created by Benjamin Descours--Terrier
//

#ifndef TO_DO_LIST_LOGGER_H
#define TO_DO_LIST_LOGGER_H
#include <fstream>
#include <string>
#include <mutex>

class Logger {
public :
    enum LogLevel {DEBUG, INFO, WARNING, ERROR};
    void log(LogLevel level, const std::string& message);
    void debug(const std::string& msg)   { log(LogLevel::DEBUG, msg); }
    void info(const std::string& msg)    { log(LogLevel::INFO, msg); }
    void warning(const std::string& msg) { log(LogLevel::WARNING, msg); }
    void error(const std::string& msg)   { log(LogLevel::ERROR, msg); }
    ~Logger();
    Logger();
private:
    std::ofstream logFile;
    std::mutex logMutex;
    std::string generateFileName();
    std::string currentTimestamp();
    std::string levelToString(LogLevel level);
};
extern Logger logger;

#endif //TO_DO_LIST_LOGGER_H