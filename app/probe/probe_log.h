// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdio>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>

namespace curia::probe {

// "key=value" with the value quoted when it holds spaces, quotes, '=' or control characters.
std::string kv(std::string_view key, std::string_view value);
std::string kv(std::string_view key, long long value);
std::string kv(std::string_view key, double value, int decimals);

// Space-separated fields.
std::string fields(std::initializer_list<std::string> parts);

// Append-only log file; every line is flushed so a killed probe still leaves a usable log.
class ProbeLog {
public:
    ProbeLog() = default;
    ProbeLog(const ProbeLog&) = delete;
    ProbeLog& operator=(const ProbeLog&) = delete;
    ~ProbeLog();

    bool open(const std::string& path, bool echo);
    void write(std::string_view event, std::string_view fieldText = {});
    const std::string& path() const { return path_; }

    // "2026-10-05 14:03:09.123"; UTC for tests, local time for the real log.
    static std::string formatTimestamp(std::chrono::system_clock::time_point time, bool utc);
    static std::string formatLine(std::string_view timestamp, std::string_view event,
                                  std::string_view fieldText);

private:
    std::FILE* file_ = nullptr;
    bool echo_ = false;
    std::string path_;
    std::mutex mutex_;
};

}  // namespace curia::probe
