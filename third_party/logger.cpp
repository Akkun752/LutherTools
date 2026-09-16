//
// Created by Benjamin Descours--Terrier
//

#include "logger.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <iomanip>

std::string Logger::levelToString(LogLevel level) {
    switch (level) {
        case DEBUG:
            return "DEBUG";
        case INFO:
            return "INFO";
        case WARNING:
            return "WARNING";
        case ERROR:
            return "ERROR";
        default:
            return "UNKNOWN";
    }
}

std::string Logger::generateFileName() {
    return "log/" + currentTimestamp() + ".log";
}

std::string Logger::currentTimestamp() {
    auto now = std::chrono::system_clock::now();
    auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()) % 1000000;

    std::time_t now_c = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now{};

#ifdef _WIN32
    localtime_s(&tm_now, &now_c);
#else
    localtime_r(&now_c, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y-%m-%d-%H-%M-%S")
        << '-' << std::setfill('0') << std::setw(6) << us.count();
    return oss.str();
}

void Logger::log(LogLevel level, const std::string &message) {
    std::string logLine = "[" + currentTimestamp() + "] [" + levelToString(level) + "] " + message;
    std::cout<< logLine << '\n';
    std::lock_guard<std::mutex> lock(logMutex);
    if (logFile.is_open()) {
        logFile << logLine << '\n';
        logFile.flush();}
    if (level == ERROR) {
        std::cerr << logLine << '\n';
    }
}

Logger::Logger() {
    std::filesystem::path logDir = "log";
    if (!std::filesystem::exists(logDir)) {
        std::filesystem::create_directories(logDir);
    }
    logFile.open(generateFileName(), std::ios::app);
    info("Creation of log file");
}

Logger::~Logger() {
    if (logFile.is_open()) {
        info("Close of App");
        logFile.close();
    }
}

Logger logger;
