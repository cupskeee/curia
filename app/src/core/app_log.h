// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <type_traits>

#include "core/redact.h"
#include "platform/env.h"

namespace curia {

// The app's own log (curia.log), one line per event:
//
//   2026-10-12T14:03:22.123Z info net_done status=200 bytes=5120 ms=840
//   <UTC time, ms>           <level> <event> <key=value ...>
//
// What it may hold: events and structured scalar fields only (sizes, counts, timings, hashes,
// status codes, short names of states). The API takes no format string and no free text on
// purpose, so the callers cannot hand it a prompt, an answer, a header, a URL with a key or an
// error body by accident; they would have to put it in a field value on purpose, and the
// source rules and the review say that is forbidden. Defence in depth for the day one does anyway:
//   * every value goes through redact() whole before it is shortened (a secret cut by the length
//     cap would otherwise leave its first bytes behind), and the finished line goes through
//     redact() again (a secret split between a key, a value and the event name);
//   * values are sanitised (control bytes, CR and LF become '?') and capped at kMaxValueBytes with
//     a trailing "...", so a log line is always exactly one line and a short one;
//   * keys and the event name keep only [A-Za-z0-9_.:-].
// A value with a blank, '=', a quote or a backslash is written in double quotes with the quote and
// backslash escaped, so a line can be split into fields again by a script.
//
// Rotation: when a line would take curia.log past maxBytes (2 MiB) the file becomes curia.log.1
// (replacing the old one) and a fresh curia.log starts. On POSIX the directory is created 0700
// and the file 0600.
//
// No exceptions: if the directory or the file cannot be opened, or a write fails, the logger turns
// into a no-op and ok() says so. Thread-safe; log() may be called from any thread.
class AppLog {
public:
    enum class Level { Debug, Info, Warn, Error };

    // One key=value pair. The key is a view into the caller's storage (a literal in practice) and
    // only has to live until log() returns; the value is copied.
    struct Field {
        std::string_view key;
        std::string value;

        Field(std::string_view k, std::string_view v) : key(k), value(v) {}
        Field(std::string_view k, const char* v) : key(k), value(v != nullptr ? v : "") {}
        Field(std::string_view k, bool v) : key(k), value(v ? "true" : "false") {}
        template <typename Integer,
                  std::enable_if_t<std::is_integral_v<Integer> && !std::is_same_v<Integer, bool>,
                                   int> = 0>
        Field(std::string_view k, Integer v) : key(k), value(std::to_string(v)) {}
    };

    static constexpr std::size_t kDefaultMaxBytes = 2 * 1024 * 1024;
    static constexpr std::size_t kMaxValueBytes = 256;
    static constexpr std::size_t kMaxNameBytes = 64;

    struct Options {
        std::size_t maxBytes = kDefaultMaxBytes;
        // Lines below this level are dropped.
        Level minLevel = Level::Debug;
        // The secrets to scrub; null means the process-wide registry. Tests pass their own.
        const SecretRegistry* registry = nullptr;
        // Time source; empty means the system clock. Tests pass a fixed time.
        std::function<std::chrono::system_clock::time_point()> clock;
    };

    // Opens <directory>/curia.log for appending, creating the directory if needed. An empty
    // directory path gives a no-op logger.
    explicit AppLog(const std::filesystem::path& directory);
    AppLog(const std::filesystem::path& directory, Options options);
    ~AppLog();

    AppLog(const AppLog&) = delete;
    AppLog& operator=(const AppLog&) = delete;

    // False if the log could not be opened or a write failed; log() then does nothing.
    bool ok() const;
    const std::filesystem::path& path() const { return path_; }

    void log(Level level, std::string_view event, std::initializer_list<Field> fields = {});

    // The log folder for this OS: macOS ~/Library/Logs/Curia; Windows %LOCALAPPDATA%\Curia\logs;
    // Linux $XDG_STATE_HOME/curia (only if absolute) else ~/.local/state/curia. Empty if the
    // variables that name the base are not set.
    static std::filesystem::path defaultLogDir();

    enum class Os { Mac, Windows, Linux };
    // The same for a chosen OS and an injected environment: pure string work, so every OS's
    // layout is testable on any machine and tests never read the real environment.
    static std::filesystem::path defaultLogDirFor(Os os, const platform::GetEnv& getenv);
    // The same for the OS this binary runs on, with an injected environment.
    static std::filesystem::path defaultLogDir(const platform::GetEnv& getenv);

    // "2026-10-12T14:03:22.123Z": ISO 8601, UTC, milliseconds, no locale or time zone database.
    static std::string formatTimestamp(std::chrono::system_clock::time_point time);

    // Exposed for tests: how a value, key or event name is turned into its log form (before the
    // final redaction of the whole line).
    static std::string sanitizeValue(std::string_view value);
    static std::string sanitizeName(std::string_view name);

private:
    struct FileCloser {
        void operator()(std::FILE* file) const noexcept {
            if (file != nullptr) {
                std::fclose(file);
            }
        }
    };

    bool openLocked();
    void rotateLocked();

    std::filesystem::path path_;
    Options options_;
    const SecretRegistry* registry_;

    mutable std::mutex mutex_;
    std::unique_ptr<std::FILE, FileCloser> file_;
    std::size_t size_ = 0;
};

}  // namespace curia
