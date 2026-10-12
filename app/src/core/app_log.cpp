// SPDX-License-Identifier: MIT
#include "core/app_log.h"

#include <algorithm>
#include <cstdint>
#include <system_error>
#include <utility>

#include "bridge/record.h"

#if !defined(_WIN32)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace curia {

namespace {

constexpr std::string_view kLogFileName = "curia.log";
constexpr std::string_view kEllipsis = "...";

const char* levelName(AppLog::Level level) {
    switch (level) {
    case AppLog::Level::Debug:
        return "debug";
    case AppLog::Level::Info:
        return "info";
    case AppLog::Level::Warn:
        return "warn";
    case AppLog::Level::Error:
        return "error";
    }
    return "info";
}

std::FILE* openAppend(const std::filesystem::path& path) {
#if defined(_WIN32)
    std::FILE* file = nullptr;
    return _wfopen_s(&file, path.c_str(), L"ab") == 0 ? file : nullptr;
#else
    // Created with mode 0600 from the start (the umask can only remove bits), and a symbolic link
    // in the place of the log is refused instead of followed to wherever it points.
    const int fd =
        ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return nullptr;
    }
    std::FILE* file = ::fdopen(fd, "ab");
    if (file == nullptr) {
        ::close(fd);
    }
    return file;
#endif
}

// Floor division, so times before 1970 also land on the right day.
std::int64_t floorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) {
        --quotient;
    }
    return quotient;
}

}  // namespace

AppLog::AppLog(const std::filesystem::path& directory) : AppLog(directory, Options()) {}

AppLog::AppLog(const std::filesystem::path& directory, Options options)
    : path_(directory.empty() ? std::filesystem::path() : directory / std::string(kLogFileName)),
      options_(std::move(options)),
      registry_(options_.registry != nullptr ? options_.registry : &SecretRegistry::instance()) {
    if (!directory.empty()) {
        const std::lock_guard<std::mutex> lock(mutex_);
        openLocked();
    }
}

AppLog::~AppLog() = default;

bool AppLog::ok() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return file_ != nullptr;
}

bool AppLog::openLocked() {
    std::error_code ec;
    const std::filesystem::path directory = path_.parent_path();
    const bool existed = std::filesystem::exists(directory, ec);
    std::filesystem::create_directories(directory, ec);
    if (ec) {
        return false;
    }
    if (!existed) {
        // Only owner access (a no-op on Windows, where the profile folder is already private).
        std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                                     std::filesystem::perm_options::replace, ec);
    }
    std::FILE* file = openAppend(path_);
    if (file == nullptr) {
        return false;
    }
    file_.reset(file);
    // Also tightens a file left by an older run with looser rights, before anything is written.
    std::filesystem::permissions(
        path_, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace, ec);
    const std::uintmax_t existing = std::filesystem::file_size(path_, ec);
    size_ = ec ? 0 : static_cast<std::size_t>(existing);
    return true;
}

void AppLog::rotateLocked() {
    file_.reset();
    std::error_code ec;
    std::filesystem::path rotated = path_;
    rotated += ".1";
    // rename() onto an existing file fails on some platforms: remove the old generation first.
    std::filesystem::remove(rotated, ec);
    std::filesystem::rename(path_, rotated, ec);
    if (ec) {
        // Keep the size bound even if the rename failed: start the file over.
        std::filesystem::remove(path_, ec);
    }
    size_ = 0;
    if (!openLocked()) {
        file_.reset();
    }
}

std::string AppLog::formatTimestamp(std::chrono::system_clock::time_point time) {
    using namespace std::chrono;
    const std::int64_t millis = duration_cast<milliseconds>(time.time_since_epoch()).count();
    std::int64_t days = floorDiv(millis, 86400000);
    const std::int64_t inDay = millis - days * 86400000;

    // Civil date from days since 1970-01-01 (proleptic Gregorian), the usual era-based method.
    days += 719468;
    const std::int64_t era = floorDiv(days, 146097);
    const std::int64_t dayOfEra = days - era * 146097;
    const std::int64_t yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
    const std::int64_t dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const std::int64_t shiftedMonth = (5 * dayOfYear + 2) / 153;
    const std::int64_t day = dayOfYear - (153 * shiftedMonth + 2) / 5 + 1;
    const std::int64_t month = shiftedMonth < 10 ? shiftedMonth + 3 : shiftedMonth - 9;
    const std::int64_t year = yearOfEra + era * 400 + (month <= 2 ? 1 : 0);

    char buffer[48];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  static_cast<long long>(year), static_cast<int>(month), static_cast<int>(day),
                  static_cast<int>(inDay / 3600000), static_cast<int>(inDay / 60000 % 60),
                  static_cast<int>(inDay / 1000 % 60), static_cast<int>(inDay % 1000));
    return buffer;
}

std::string AppLog::sanitizeName(std::string_view name) {
    std::string out;
    out.reserve(std::min(name.size(), kMaxNameBytes));
    for (const char ch : name) {
        if (out.size() >= kMaxNameBytes) {
            break;
        }
        const bool keep = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                          (ch >= '0' && ch <= '9') || ch == '_' || ch == '.' || ch == ':' ||
                          ch == '-';
        out.push_back(keep ? ch : '_');
    }
    if (out.empty()) {
        out = "_";
    }
    return out;
}

std::string AppLog::sanitizeValue(std::string_view value) {
    std::string text(value);
    for (char& ch : text) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte < 0x20 || byte == 0x7f) {
            ch = '?';
        }
    }
    if (text.size() > kMaxValueBytes) {
        // Cut on a character boundary where the bytes are UTF-8, then mark the cut.
        const std::string_view head =
            bridge::trimIncompleteUtf8Tail(std::string_view(text).substr(0, kMaxValueBytes - 3));
        text = std::string(head) + std::string(kEllipsis);
    }
    const bool needsQuotes = text.empty() || text.find_first_of(" =\"\\") != std::string::npos;
    if (!needsQuotes) {
        return text;
    }
    std::string quoted = "\"";
    for (const char ch : text) {
        if (ch == '"' || ch == '\\') {
            quoted.push_back('\\');
        }
        quoted.push_back(ch);
    }
    quoted.push_back('"');
    return quoted;
}

void AppLog::log(Level level, std::string_view event, std::initializer_list<Field> fields) {
    if (level < options_.minLevel) {
        return;
    }
    {
        // Nothing to format for a logger that is off.
        const std::lock_guard<std::mutex> lock(mutex_);
        if (file_ == nullptr) {
            return;
        }
    }
    const auto now = options_.clock ? options_.clock() : std::chrono::system_clock::now();
    std::string line = formatTimestamp(now);
    line.push_back(' ');
    line += levelName(level);
    line.push_back(' ');
    line += sanitizeName(registry_->redact(event));
    for (const Field& field : fields) {
        line.push_back(' ');
        line += sanitizeName(registry_->redact(field.key));
        line.push_back('=');
        // Redact the whole value first: a secret cut by the length cap would leave its start.
        line += sanitizeValue(registry_->redact(field.value));
    }
    line = registry_->redact(line);
    line.push_back('\n');

    const std::lock_guard<std::mutex> lock(mutex_);
    if (file_ == nullptr) {
        return;
    }
    if (size_ > 0 && size_ + line.size() > options_.maxBytes) {
        rotateLocked();
        if (file_ == nullptr) {
            return;
        }
    }
    if (std::fwrite(line.data(), 1, line.size(), file_.get()) != line.size() ||
        std::fflush(file_.get()) != 0) {
        file_.reset();  // a full disk or a vanished folder: stop trying
        return;
    }
    size_ += line.size();
}

std::filesystem::path AppLog::defaultLogDirFor(Os os, const platform::GetEnv& getenv) {
    if (!getenv) {
        return {};
    }
    switch (os) {
    case Os::Mac: {
        const std::string home = getenv("HOME");
        return home.empty() ? std::filesystem::path()
                            : std::filesystem::path(home) / "Library" / "Logs" / "Curia";
    }
    case Os::Windows: {
        const std::string base = getenv("LOCALAPPDATA");
        return base.empty() ? std::filesystem::path()
                            : std::filesystem::path(base) / "Curia" / "logs";
    }
    case Os::Linux: {
        // The XDG specification says a relative value must be ignored.
        const std::string state = getenv("XDG_STATE_HOME");
        if (!state.empty() && state.front() == '/') {
            return std::filesystem::path(state) / "curia";
        }
        const std::string home = getenv("HOME");
        return home.empty() ? std::filesystem::path()
                            : std::filesystem::path(home) / ".local" / "state" / "curia";
    }
    }
    return {};
}

std::filesystem::path AppLog::defaultLogDir(const platform::GetEnv& getenv) {
#if defined(_WIN32)
    return defaultLogDirFor(Os::Windows, getenv);
#elif defined(__APPLE__)
    return defaultLogDirFor(Os::Mac, getenv);
#else
    return defaultLogDirFor(Os::Linux, getenv);
#endif
}

std::filesystem::path AppLog::defaultLogDir() {
#if defined(_WIN32)
    // The wide variable, so a profile path with non-ASCII characters survives.
    const std::filesystem::path base = platform::getEnvPath(L"LOCALAPPDATA", "LOCALAPPDATA");
    return base.empty() ? std::filesystem::path() : base / "Curia" / "logs";
#else
    return defaultLogDir(platform::GetEnv(platform::getEnv));
#endif
}

}  // namespace curia
