// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bridge/bridge_pipeline.h"
#include "bridge/events.h"

namespace curia::bridge {

// What the tailer needs from a file. A fake in the unit tests, FileSource in the app. Methods are
// called from one thread at a time.
class IFileSource {
public:
    virtual ~IFileSource() = default;

    enum class State {
        Missing,  // the file does not exist
        Ok,
        Error,  // exists (or may exist) but cannot be examined
    };
    struct Stat {
        State state = State::Missing;
        std::uint64_t size = 0;  // valid when state is Ok
    };

    virtual Stat stat() = 0;

    // Appends up to `maxBytes` bytes starting at `offset` to `out`; returns false when the file
    // cannot be read. Fewer bytes than asked (also none) is not an error: the file may have shrunk
    // since stat().
    virtual bool read(std::uint64_t offset, std::size_t maxBytes, std::string& out) = 0;
};

// The real file, opened read-only for every call and closed again, so that the tailer never holds
// the file: CK3 can truncate or rewrite it at any time and the game must never wait for Curia
// (NFR-5). std::ifstream opens with shared read/write access on MSVC, glibc and libc++; whether
// CK3's own handle on Windows allows that has NOT been verified (no Windows test exists). The size
// is taken from an open handle (seek to the end), not from directory metadata, which NTFS can
// report stale for a file that another process is still writing.
class FileSource final : public IFileSource {
public:
    // A path from the command line.
    explicit FileSource(std::string path) : path_(std::move(path)) {}
    // A path as the OS spells it (a folder from the platform layer): never narrowed to a byte
    // string, so that on Windows a user folder with characters outside the ANSI code page works.
    explicit FileSource(std::filesystem::path path) : path_(std::move(path)) {}
    Stat stat() override;
    bool read(std::uint64_t offset, std::size_t maxBytes, std::string& out) override;

private:
    std::filesystem::path path_;
};

// Something worth telling the app's log about; none of these is an error of the bridge.
enum class TailerNotice {
    // First successful look at the file (at its end, or at the start when fromStart or when the
    // file appeared after the tailer started).
    Attached,
    // The file shrank (the game truncates it in place at launch): the splitter and the assembler
    // were told and reading restarted at offset 0.
    Truncated,
    // The file does not exist (at start, or it was deleted). Reported once per absence.
    Missing,
    // The file exists but cannot be read. Reported once per failure episode.
    Unreadable,
    // The file can be read again after Missing or Unreadable.
    Recovered,
};

const char* toString(TailerNotice notice) noexcept;

struct TailerStats {
    // The next byte to read.
    std::uint64_t offset = 0;
    // Bytes read since the tailer attached, reset to 0 on a truncation.
    std::uint64_t totalBytes = 0;
    // Bytes of Curia lines among them (BridgePipeline::curiaLineBytes), reset with the above.
    std::uint64_t curiaBytes = 0;
    std::uint64_t truncations = 0;
};

struct PollResult {
    std::vector<Event> events;
    std::vector<TailerNotice> notices;
    // Bytes read by this poll.
    std::uint64_t bytesRead = 0;
};

struct TailerOptions {
    // Read an existing file from its first byte instead of attaching at its end (tests, tools).
    bool fromStart = false;
};

// Polling tail of one file feeding a BridgePipeline. No threads and no clock: the caller calls
// poll() whenever it likes (LogWatcher does it on a timer).
//
// Behaviour:
//  - Attaches at the END of an existing file (history is not replayed) unless Options::fromStart.
//    A file that does not exist yet is read from offset 0 when it appears.
//  - Reads only bytes appended since the last poll, at most kChunkBytes per read call, so the
//    pipeline never sees more than that at once.
//  - A size below the offset is a truncation. A truncation followed by growth beyond the old
//    offset between two polls cannot be told from an append (there is no inode or mtime check):
//    a known limit, harmless in practice because the game writes little right after launch.
//  - A file that disappears is treated like a truncation, then waited for.
class LogTailer {
public:
    static constexpr std::size_t kChunkBytes = 64 * 1024;

    using Options = TailerOptions;

    explicit LogTailer(std::unique_ptr<IFileSource> source, Options options = {});

    PollResult poll();

    TailerStats stats() const noexcept;
    const BridgePipeline& pipeline() const noexcept { return pipeline_; }

private:
    void onTruncation(PollResult& result);
    // Reads up to `size`; false when a read failed (the bytes stay unread for the next poll).
    bool readAppended(std::uint64_t size, PollResult& result);

    std::unique_ptr<IFileSource> source_;
    Options options_;
    BridgePipeline pipeline_;
    // offset_ is valid (the file was seen and not lost since).
    bool attached_ = false;
    // The next attach goes to the end of the file: only for a file that existed at the first poll.
    bool attachAtEnd_ = true;
    // Episode flags, so that each Missing or Unreadable episode is reported once.
    bool missing_ = false;
    bool unreadable_ = false;
    std::uint64_t offset_ = 0;
    std::uint64_t totalBytes_ = 0;
    std::uint64_t curiaBytes_ = 0;
    std::uint64_t truncations_ = 0;
};

}  // namespace curia::bridge
