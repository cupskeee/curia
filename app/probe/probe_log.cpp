// SPDX-License-Identifier: MIT
#include "probe_log.h"

#include <ctime>

namespace curia::probe {

namespace {

bool needsQuotes(std::string_view value) {
    if (value.empty()) {
        return true;
    }
    for (const char c : value) {
        if (c == ' ' || c == '"' || c == '=' || static_cast<unsigned char>(c) < 0x20U) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string kv(std::string_view key, std::string_view value) {
    std::string out(key);
    out += '=';
    if (!needsQuotes(value)) {
        out += value;
        return out;
    }
    out += '"';
    for (const char c : value) {
        if (c == '"') {
            out += "\\\"";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\\') {
            out += "\\\\";
        } else if (static_cast<unsigned char>(c) < 0x20U) {
            out += ' ';
        } else {
            out += c;
        }
    }
    out += '"';
    return out;
}

std::string kv(std::string_view key, long long value) {
    return std::string(key) + "=" + std::to_string(value);
}

std::string kv(std::string_view key, double value, int decimals) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%.*f", decimals, value);
    return std::string(key) + "=" + buffer;
}

std::string fields(std::initializer_list<std::string> parts) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) {
            out += ' ';
        }
        out += part;
    }
    return out;
}

ProbeLog::~ProbeLog() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

bool ProbeLog::open(const std::string& path, bool echo) {
    const std::lock_guard<std::mutex> lock(mutex_);
    file_ = std::fopen(path.c_str(), "a");
    path_ = path;
    echo_ = echo;
    return file_ != nullptr;
}

void ProbeLog::write(std::string_view event, std::string_view fieldText) {
    const std::string line =
        formatLine(formatTimestamp(std::chrono::system_clock::now(), false), event, fieldText);
    const std::lock_guard<std::mutex> lock(mutex_);
    if (file_ != nullptr) {
        std::fputs(line.c_str(), file_);
        std::fflush(file_);
    }
    if (echo_) {
        std::fputs(line.c_str(), stderr);
    }
}

std::string ProbeLog::formatTimestamp(std::chrono::system_clock::time_point time, bool utc) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
                            time - std::chrono::system_clock::from_time_t(seconds))
                            .count();
    std::tm parts{};
    if (utc) {
        gmtime_r(&seconds, &parts);
    } else {
        localtime_r(&seconds, &parts);
    }
    char buffer[40];
    std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", &parts);
    char out[48];
    std::snprintf(out, sizeof out, "%s.%03lld", buffer, static_cast<long long>(millis));
    return out;
}

std::string ProbeLog::formatLine(std::string_view timestamp, std::string_view event,
                                 std::string_view fieldText) {
    std::string line(timestamp);
    line += ' ';
    line += event;
    if (!fieldText.empty()) {
        line += ' ';
        line += fieldText;
    }
    line += '\n';
    return line;
}

}  // namespace curia::probe
