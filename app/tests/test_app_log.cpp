// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/app_log.h"

using curia::AppLog;
using curia::SecretRegistry;

namespace fs = std::filesystem;

namespace {

// A fresh folder under the system temp directory, removed (with its content) at the end of the
// test. Tests never touch the real log folder of the user.
class TempDir {
public:
    TempDir() {
        std::random_device device;
        std::mt19937_64 rng(device());
        path_ = fs::temp_directory_path() / ("curia_test_applog_" + std::to_string(rng()));
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

std::string readFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::string> readLines(const fs::path& path) {
    std::vector<std::string> lines;
    std::istringstream in(readFile(path));
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    return lines;
}

std::chrono::system_clock::time_point at(std::int64_t millis) {
    return std::chrono::system_clock::time_point(
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
            std::chrono::milliseconds(millis)));
}

AppLog::Options fixedOptions(const SecretRegistry* registry) {
    AppLog::Options options;
    options.registry = registry;
    options.clock = [] { return at(1760278402123); };
    return options;
}

constexpr const char* kStamp = "2025-10-12T14:13:22.123Z";

std::string withStamp(const std::string& rest) {
    return std::string(kStamp) + " " + rest;
}

std::map<std::string, std::string> envMap;

std::string fromMap(const char* name) {
    const auto found = envMap.find(name);
    return found != envMap.end() ? found->second : std::string();
}

}  // namespace

TEST_CASE("app log: timestamps are ISO 8601 UTC with milliseconds") {
    CHECK(AppLog::formatTimestamp(at(0)) == "1970-01-01T00:00:00.000Z");
    CHECK(AppLog::formatTimestamp(at(1760278402123)) == kStamp);
    CHECK(AppLog::formatTimestamp(at(951782400000)) == "2000-02-29T00:00:00.000Z");
    CHECK(AppLog::formatTimestamp(at(4107542400000)) == "2100-03-01T00:00:00.000Z");
    CHECK(AppLog::formatTimestamp(at(1000000000000)) == "2001-09-09T01:46:40.000Z");
    CHECK(AppLog::formatTimestamp(at(-1)) == "1969-12-31T23:59:59.999Z");
}

TEST_CASE("app log: one line per event, key=value fields") {
    TempDir dir;
    SecretRegistry registry;
    {
        AppLog log(dir.path(), fixedOptions(&registry));
        REQUIRE(log.ok());
        log.log(AppLog::Level::Info, "net_done", {{"status", 200}, {"bytes", 5120u}, {"ok", true}});
        log.log(AppLog::Level::Warn, "retry", {{"phase", "connect"}, {"ms", std::int64_t{-5}}});
        log.log(AppLog::Level::Debug, "tick");
        log.log(AppLog::Level::Error, "failed", {{"reason", std::string("timeout")}});
    }
    const std::vector<std::string> lines = readLines(dir.path() / "curia.log");
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == withStamp("info net_done status=200 bytes=5120 ok=true"));
    CHECK(lines[1] == withStamp("warn retry phase=connect ms=-5"));
    CHECK(lines[2] == withStamp("debug tick"));
    CHECK(lines[3] == withStamp("error failed reason=timeout"));
}

TEST_CASE("app log: the folder is created, the file appended to") {
    TempDir dir;
    const fs::path nested = dir.path() / "a" / "b";
    SecretRegistry registry;
    {
        AppLog log(nested, fixedOptions(&registry));
        REQUIRE(log.ok());
        CHECK(log.path() == nested / "curia.log");
        log.log(AppLog::Level::Info, "first");
    }
    {
        AppLog log(nested, fixedOptions(&registry));
        REQUIRE(log.ok());
        log.log(AppLog::Level::Info, "second");
    }
    const std::vector<std::string> lines = readLines(nested / "curia.log");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == withStamp("info first"));
    CHECK(lines[1] == withStamp("info second"));
}

TEST_CASE("app log: values are sanitised and quoted") {
    CHECK(AppLog::sanitizeValue("plain") == "plain");
    CHECK(AppLog::sanitizeValue("a\r\nb") == "a??b");
    CHECK(AppLog::sanitizeValue(std::string("a\0b\x01\x1f\x7f", 6)) == "a?b???");
    CHECK(AppLog::sanitizeValue("two words") == "\"two words\"");
    CHECK(AppLog::sanitizeValue("k=v") == "\"k=v\"");
    CHECK(AppLog::sanitizeValue("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(AppLog::sanitizeValue("back\\slash") == "\"back\\\\slash\"");
    CHECK(AppLog::sanitizeValue("") == "\"\"");
    CHECK(AppLog::sanitizeValue("caf\xc3\xa9") == "caf\xc3\xa9");  // UTF-8 passes through

    CHECK(AppLog::sanitizeName("good.name_1:x-y") == "good.name_1:x-y");
    CHECK(AppLog::sanitizeName("bad name=\n") == "bad_name__");
    CHECK(AppLog::sanitizeName("") == "_");
    CHECK(AppLog::sanitizeName(std::string(200, 'n')).size() == AppLog::kMaxNameBytes);
}

TEST_CASE("app log: long values are cut at 256 bytes with an ellipsis") {
    const std::string exact(AppLog::kMaxValueBytes, 'x');
    CHECK(AppLog::sanitizeValue(exact) == exact);
    const std::string cut = AppLog::sanitizeValue(std::string(300, 'x'));
    CHECK(cut.size() == AppLog::kMaxValueBytes);
    CHECK(cut == std::string(AppLog::kMaxValueBytes - 3, 'x') + "...");

    // The cut never splits a UTF-8 sequence: 253 bytes would end inside the last "e acute".
    std::string text(AppLog::kMaxValueBytes - 4, 'a');
    text += "\xc3\xa9\xc3\xa9\xc3\xa9";
    const std::string trimmed = AppLog::sanitizeValue(text);
    CHECK(trimmed.size() < AppLog::kMaxValueBytes);
    CHECK(trimmed.substr(trimmed.size() - 3) == "...");
    const std::string body = trimmed.substr(0, trimmed.size() - 3);
    CHECK(body == std::string(AppLog::kMaxValueBytes - 4, 'a'));
}

TEST_CASE("app log: a log line is always exactly one line") {
    TempDir dir;
    SecretRegistry registry;
    {
        AppLog log(dir.path(), fixedOptions(&registry));
        log.log(AppLog::Level::Info, "evil\nevent", {{"k\r\ney", "v1\nv2\r\nv3"}, {"x", "\n\n\n"}});
    }
    const std::string text = readFile(dir.path() / "curia.log");
    CHECK(std::count(text.begin(), text.end(), '\n') == 1);
    CHECK(text.find('\r') == std::string::npos);
    CHECK(text == withStamp("info evil_event k__ey=v1?v2??v3 x=???\n"));
}

TEST_CASE("app log: registered secrets never reach the file") {
    TempDir dir;
    SecretRegistry registry;
    const std::string secret = std::string("sk-ant-") + "FAKE" + "abcdefghijklmnopqrstuvwxyz";
    const std::string other = "plain-registered-secret";
    REQUIRE(registry.add(secret));
    REQUIRE(registry.add(other));
    {
        AppLog log(dir.path(), fixedOptions(&registry));
        log.log(AppLog::Level::Info, "ev_" + secret, {{"value", secret}});
        log.log(AppLog::Level::Info, other, {{other, other}, {"url", "https://h/?k=" + other}});
        // The secret straddles the 256-byte cap (it starts at byte 240, the cut is at 253):
        // cutting before scrubbing would leave its first 13 bytes behind.
        log.log(AppLog::Level::Info, "cap",
                {{"v", std::string(240, 'p') + other + std::string(50, 'q')}});
        log.log(AppLog::Level::Info, "hdr",
                {{"h", "authorization: Bearer abc"}, {"b", "Bearer tok"}});
        log.log(AppLog::Level::Info, "json", {{"body", "{\"key\":\"" + other + "\"}"}});
    }
    const std::string text = readFile(dir.path() / "curia.log");
    CHECK(text.find(secret) == std::string::npos);
    CHECK(text.find(secret.substr(0, 12)) == std::string::npos);
    CHECK(text.find(other) == std::string::npos);
    CHECK(text.find(other.substr(0, 10)) == std::string::npos);
    CHECK(text.find("abc") == std::string::npos);  // the header value
    CHECK(text.find("tok") == std::string::npos);  // the bearer token
    CHECK(text.find("[redacted]") != std::string::npos);
    CHECK(std::count(text.begin(), text.end(), '\n') == 5);
}

TEST_CASE("app log: each stage of the pipeline scrubs on its own") {
    // One secret per case, placed where only one stage can catch it. The stable text around it
    // proves the line was written; the secret's pieces must be gone.
    SecretRegistry registry;
    const std::string secret = "abcdEFGHijklMNOP";  // 16 bytes
    REQUIRE(registry.add(secret));

    const auto written = [&](SecretRegistry& scrubber, std::string_view event,
                             std::initializer_list<AppLog::Field> fields) {
        TempDir dir;
        {
            AppLog log(dir.path(), fixedOptions(&scrubber));
            log.log(AppLog::Level::Info, event, fields);
        }
        return readFile(dir.path() / "curia.log");
    };

    // Value: straddles the cap, so only the value-level scrub (before the cut) can remove it.
    std::string text = written(registry, "v", {{"k", std::string(245, 'p') + secret + "zz"}});
    CHECK(text.find("pppp") != std::string::npos);
    CHECK(text.find("EFGH") == std::string::npos);
    CHECK(text.find("abcdE") == std::string::npos);

    // Key and event name: the secret holds bytes that sanitizeName rewrites to '_', so after
    // sanitising, the finished line no longer contains it and only the earlier scrub can help.
    const std::string odd = "ab cd=ef gh ij";
    REQUIRE(registry.add(odd));
    text = written(registry, "e", {{"x" + odd, "val"}});
    CHECK(text.find("ab_cd") == std::string::npos);
    CHECK(text.find("ef_gh") == std::string::npos);
    CHECK(text.find("=val") != std::string::npos);
    text = written(registry, "ev" + odd, {{"k", "val"}});
    CHECK(text.find("ab_cd") == std::string::npos);
    CHECK(text.find("ef_gh") == std::string::npos);
    CHECK(text.find("k=val") != std::string::npos);

    // Split over key and value: no earlier stage sees the whole secret, only the finished line.
    SecretRegistry splitRegistry;
    REQUIRE(splitRegistry.add("abcd=efghijkl"));
    text = written(splitRegistry, "s", {{"abcd", "efghijkl"}});
    CHECK(text.find("efgh") == std::string::npos);
    CHECK(text.find("abcd") == std::string::npos);
    CHECK(text.find("[redacted]") != std::string::npos);
}

TEST_CASE("app log: the process-wide registry is used by default") {
    TempDir dir;
    const std::string secret = "process-registry-secret-value";
    REQUIRE(SecretRegistry::instance().add(secret));
    {
        AppLog::Options options;
        options.clock = [] { return at(0); };
        AppLog log(dir.path(), options);
        log.log(AppLog::Level::Info, "x", {{"v", secret}});
    }
    SecretRegistry::instance().remove(secret);
    CHECK(readFile(dir.path() / "curia.log") == "1970-01-01T00:00:00.000Z info x v=[redacted]\n");
}

TEST_CASE("app log: minimum level") {
    TempDir dir;
    SecretRegistry registry;
    {
        AppLog::Options options = fixedOptions(&registry);
        options.minLevel = AppLog::Level::Warn;
        AppLog log(dir.path(), options);
        log.log(AppLog::Level::Debug, "d");
        log.log(AppLog::Level::Info, "i");
        log.log(AppLog::Level::Warn, "w");
        log.log(AppLog::Level::Error, "e");
    }
    const std::vector<std::string> lines = readLines(dir.path() / "curia.log");
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == withStamp("warn w"));
    CHECK(lines[1] == withStamp("error e"));
}

TEST_CASE("app log: rotation at the size limit") {
    TempDir dir;
    SecretRegistry registry;
    AppLog::Options options = fixedOptions(&registry);
    options.maxBytes = 400;
    {
        AppLog log(dir.path(), options);
        REQUIRE(log.ok());
        for (int i = 0; i < 60; ++i) {
            log.log(AppLog::Level::Info, "line", {{"n", i}});
            CHECK(fs::file_size(dir.path() / "curia.log") <= options.maxBytes);
        }
        CHECK(log.ok());
    }
    REQUIRE(fs::exists(dir.path() / "curia.log.1"));
    CHECK(fs::file_size(dir.path() / "curia.log.1") <= options.maxBytes);
    const std::vector<std::string> current = readLines(dir.path() / "curia.log");
    const std::vector<std::string> previous = readLines(dir.path() / "curia.log.1");
    REQUIRE_FALSE(current.empty());
    REQUIRE_FALSE(previous.empty());
    // The newest line is in the current file, and the older generation directly precedes it.
    CHECK(current.back() == withStamp("info line n=59"));
    const int firstCurrent = std::stoi(current.front().substr(current.front().rfind('=') + 1));
    const int lastPrevious = std::stoi(previous.back().substr(previous.back().rfind('=') + 1));
    CHECK(lastPrevious + 1 == firstCurrent);
    // Only two generations exist.
    CHECK_FALSE(fs::exists(dir.path() / "curia.log.2"));
}

TEST_CASE("app log: an existing file counts towards the limit") {
    TempDir dir;
    SecretRegistry registry;
    fs::create_directories(dir.path());
    {
        std::ofstream seed(dir.path() / "curia.log", std::ios::binary);
        seed << std::string(390, 'o') << '\n';
    }
    AppLog::Options options = fixedOptions(&registry);
    options.maxBytes = 420;
    {
        AppLog log(dir.path(), options);
        REQUIRE(log.ok());
        log.log(AppLog::Level::Info, "after");
    }
    REQUIRE(fs::exists(dir.path() / "curia.log.1"));
    CHECK(readLines(dir.path() / "curia.log.1").front() == std::string(390, 'o'));
    CHECK(readLines(dir.path() / "curia.log").front() == withStamp("info after"));
}

TEST_CASE("app log: a log that cannot be opened is a quiet no-op") {
    TempDir dir;
    SecretRegistry registry;
    fs::create_directories(dir.path());
    // The "folder" is a regular file.
    const fs::path blocker = dir.path() / "blocker";
    {
        std::ofstream file(blocker);
        file << "x";
    }
    AppLog broken(blocker, fixedOptions(&registry));
    CHECK_FALSE(broken.ok());
    broken.log(AppLog::Level::Error, "ignored", {{"a", 1}});  // must not crash or throw
    CHECK(fs::is_regular_file(blocker));

    AppLog empty{fs::path()};
    CHECK_FALSE(empty.ok());
    empty.log(AppLog::Level::Info, "ignored");
}

TEST_CASE("app log: concurrent writers produce whole lines") {
    TempDir dir;
    SecretRegistry registry;
    constexpr int kThreads = 4;
    constexpr int kLines = 200;
    {
        AppLog log(dir.path(), fixedOptions(&registry));
        REQUIRE(log.ok());
        std::vector<std::thread> threads;
        for (int t = 0; t < kThreads; ++t) {
            threads.emplace_back([&log, t] {
                for (int i = 0; i < kLines; ++i) {
                    log.log(AppLog::Level::Info, "work", {{"thread", t}, {"i", i}});
                }
            });
        }
        for (std::thread& thread : threads) {
            thread.join();
        }
    }
    const std::vector<std::string> lines = readLines(dir.path() / "curia.log");
    CHECK(lines.size() == static_cast<std::size_t>(kThreads * kLines));
    for (const std::string& line : lines) {
        REQUIRE(line.rfind(withStamp("info work thread="), 0) == 0);
    }
}

#if !defined(_WIN32)
TEST_CASE("app log: the folder is private to the owner on POSIX") {
    TempDir dir;
    SecretRegistry registry;
    const fs::path nested = dir.path() / "private";
    {
        AppLog log(nested, fixedOptions(&registry));
        REQUIRE(log.ok());
        log.log(AppLog::Level::Info, "x");
    }
    const auto dirPerms = fs::status(nested).permissions() & fs::perms::mask;
    const auto filePerms = fs::status(nested / "curia.log").permissions() & fs::perms::mask;
    CHECK(dirPerms == fs::perms::owner_all);
    CHECK(filePerms == (fs::perms::owner_read | fs::perms::owner_write));
}

TEST_CASE("app log: a symbolic link in place of the log is refused, not followed") {
    TempDir dir;
    SecretRegistry registry;
    const fs::path target = dir.path() / "elsewhere.txt";
    std::error_code ec;
    fs::create_directories(dir.path(), ec);
    {
        std::ofstream out(target);
        out << "keep\n";
    }
    fs::create_symlink(target, dir.path() / "curia.log", ec);
    REQUIRE_FALSE(ec);
    AppLog log(dir.path(), fixedOptions(&registry));
    CHECK_FALSE(log.ok());
    log.log(AppLog::Level::Info, "x");
    CHECK(readFile(target) == "keep\n");
}
#endif

TEST_CASE("app log: default folder per OS from an injected environment") {
    using Os = AppLog::Os;
    envMap = {{"HOME", "/home/someone"},
              {"LOCALAPPDATA", "C:/Profiles/p1/AppData/Local"},
              {"XDG_STATE_HOME", "/state/dir"}};
    CHECK(AppLog::defaultLogDirFor(Os::Mac, fromMap).generic_string() ==
          "/home/someone/Library/Logs/Curia");
    CHECK(AppLog::defaultLogDirFor(Os::Windows, fromMap).generic_string() ==
          "C:/Profiles/p1/AppData/Local/Curia/logs");
    CHECK(AppLog::defaultLogDirFor(Os::Linux, fromMap).generic_string() == "/state/dir/curia");

    envMap["XDG_STATE_HOME"] = "relative/state";  // ignored, as the XDG specification says
    CHECK(AppLog::defaultLogDirFor(Os::Linux, fromMap).generic_string() ==
          "/home/someone/.local/state/curia");
    envMap["XDG_STATE_HOME"] = "";
    CHECK(AppLog::defaultLogDirFor(Os::Linux, fromMap).generic_string() ==
          "/home/someone/.local/state/curia");

    // The overload for this OS picks the layout of the OS the test runs on.
    envMap = {{"HOME", "/home/someone"},
              {"LOCALAPPDATA", "C:/Profiles/p1/AppData/Local"},
              {"XDG_STATE_HOME", ""}};
#if defined(_WIN32)
    CHECK(AppLog::defaultLogDir(fromMap).generic_string() ==
          "C:/Profiles/p1/AppData/Local/Curia/logs");
#elif defined(__APPLE__)
    CHECK(AppLog::defaultLogDir(fromMap).generic_string() == "/home/someone/Library/Logs/Curia");
#else
    CHECK(AppLog::defaultLogDir(fromMap).generic_string() == "/home/someone/.local/state/curia");
#endif

    envMap.clear();
    CHECK(AppLog::defaultLogDir(fromMap).empty());
    CHECK(AppLog::defaultLogDirFor(Os::Mac, fromMap).empty());
    CHECK(AppLog::defaultLogDirFor(Os::Windows, fromMap).empty());
    CHECK(AppLog::defaultLogDirFor(Os::Linux, fromMap).empty());
    CHECK(AppLog::defaultLogDirFor(Os::Linux, curia::platform::GetEnv()).empty());
}

TEST_CASE("app log: the real default folder has the shape of this OS (nothing is created)") {
    const fs::path dir = AppLog::defaultLogDir();
    if (dir.empty()) {
        return;  // HOME or LOCALAPPDATA not set in this environment
    }
    const std::string text = dir.generic_string();
#if defined(_WIN32)
    CHECK(text.size() > 10);
    CHECK(text.substr(text.size() - 10) == "Curia/logs");
#elif defined(__APPLE__)
    CHECK(text.substr(text.size() - 18) == "Library/Logs/Curia");
#else
    CHECK(text.substr(text.size() - 6) == "/curia");
#endif
}
