// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bridge/bridge_pipeline.h"
#include "bridge/events.h"
#include "bridge/log_tailer.h"
#include "bridge/record.h"

// Helpers shared by the test_bridge_*.cpp files: the synthetic engine prefix, the fixture loader
// (format in tests/fixtures/curia1/README.md), event accessors.
namespace curia::bridge::test {

// A synthetic engine-style prefix of 120 to 131 bytes, built from parts at run time so that no
// engine-looking line is ever stored in a tracked file. It never contains the marker.
std::string enginePrefix(std::size_t variant = 0);

// prefix + bare record, without the newline.
std::string prefixed(std::string_view bareRecord, std::size_t variant = 0);

// prefix + bare record + '\n'.
std::string prefixedLine(std::string_view bareRecord, std::size_t variant = 0);

// parseLine on prefixed(bare).
ParseResult parseBare(std::string_view bareRecord, bool overlong = false);

// Concatenated prefixedLine() of each record.
std::string stream(const std::vector<std::string>& bareRecords);

// The example frame of the spec (section 4) with the given snap; the END count is correct.
std::vector<std::string> exampleFrame(std::int32_t snap);

// A fixture: bytes to append to the log, or a truncation of the log.
struct Action {
    enum class Kind { Bytes, Truncate };
    Kind kind = Kind::Bytes;
    std::string bytes;
};

struct Fixture {
    std::string name;
    std::vector<Action> actions;
    // Directive lines the loader did not understand (must be empty).
    std::vector<std::string> errors;
};

std::string fixtureDir();
// Basenames (without ".log") of every fixture, sorted.
std::vector<std::string> fixtureNames();
Fixture loadFixture(const std::string& name);
std::string readFile(const std::string& path, bool& ok);
bool writeFile(const std::string& path, const std::string& contents);

// How runFixture splits the bytes of an action: chunkSize 0 = all at once.
std::vector<Event> runActions(const std::vector<Action>& actions, std::size_t chunkSize,
                              std::uint32_t randomSeed = 0);
std::vector<Event> runBytes(std::string_view bytes);

// The golden form of a list of events.
std::string eventsText(const std::vector<Event>& events);

// A file kept in memory, shared between the test (which grows, truncates and breaks it) and the
// FakeFileSource the tailer reads it through. Thread-safe: the watcher tests use it from two
// threads.
class FakeFile {
public:
    void append(std::string_view bytes);
    // The file keeps existing with size 0 (what the game does at launch).
    void truncate();
    // The file disappears.
    void remove();
    void create();
    void setReadError(bool on);
    void setStatError(bool on);
    std::uint64_t size() const;

    // What the tailer asked of it.
    std::size_t largestRead() const;
    std::size_t readCalls() const;
    std::size_t statCalls() const;

    IFileSource::Stat stat();
    bool read(std::uint64_t offset, std::size_t maxBytes, std::string& out);

private:
    mutable std::mutex mutex_;
    bool exists_ = true;
    bool readError_ = false;
    bool statError_ = false;
    std::string content_;
    std::size_t largestRead_ = 0;
    std::size_t readCalls_ = 0;
    std::size_t statCalls_ = 0;
};

class FakeFileSource final : public IFileSource {
public:
    explicit FakeFileSource(std::shared_ptr<FakeFile> file) : file_(std::move(file)) {}
    Stat stat() override { return file_->stat(); }
    bool read(std::uint64_t offset, std::size_t maxBytes, std::string& out) override {
        return file_->read(offset, maxBytes, out);
    }

private:
    std::shared_ptr<FakeFile> file_;
};

// A scratch directory under the system temp folder, removed (with its contents) on destruction.
class TempDir {
public:
    TempDir();
    ~TempDir();
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    std::string path(const std::string& name) const;

private:
    std::string dir_;
};

// The value of an optional that the test has already shown to be there; fails the test case
// (an exception) otherwise. Written so that static analysis sees the check.
template <typename T> const T& must(const std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::runtime_error("expected a value");
    }
    return *value;
}

template <typename T> std::vector<const T*> eventsOfType(const std::vector<Event>& events) {
    std::vector<const T*> out;
    for (const Event& e : events) {
        if (const T* p = std::get_if<T>(&e)) {
            out.push_back(p);
        }
    }
    return out;
}

// The only event, which must be a T (the check is reported by the caller's REQUIRE on the result).
template <typename T> const T* onlyEvent(const std::vector<Event>& events) {
    if (events.size() != 1) {
        return nullptr;
    }
    return std::get_if<T>(&events.front());
}

}  // namespace curia::bridge::test
