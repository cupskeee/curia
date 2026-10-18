// SPDX-License-Identifier: MIT
#include "bridge/log_tailer.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <system_error>
#include <utility>

namespace curia::bridge {

const char* toString(TailerNotice notice) noexcept {
    switch (notice) {
    case TailerNotice::Attached:
        return "attached";
    case TailerNotice::Truncated:
        return "truncated";
    case TailerNotice::Missing:
        return "missing";
    case TailerNotice::Unreadable:
        return "unreadable";
    case TailerNotice::Recovered:
        return "recovered";
    }
    return "unknown";
}

IFileSource::Stat FileSource::stat() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        // Could not open: tell "not there" from "there but refused" (permissions, a lock).
        std::error_code ec;
        const bool exists = std::filesystem::exists(path_, ec);
        return {!ec && !exists ? State::Missing : State::Error, 0};
    }
    // POSIX lets ifstream open a directory (and seekg then returns a meaningless size) where
    // Windows refuses; make every platform report "exists but cannot be tailed", so that a log
    // path that points at the logs folder is told to the user instead of watched silently.
    std::error_code regularEc;
    if (!std::filesystem::is_regular_file(path_, regularEc) || regularEc) {
        return {State::Error, 0};
    }
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (!in || end < 0) {
        return {State::Error, 0};
    }
    return {State::Ok, static_cast<std::uint64_t>(end)};
}

bool FileSource::read(std::uint64_t offset, std::size_t maxBytes, std::string& out) {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        return false;
    }
    in.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!in) {
        return false;
    }
    const std::size_t before = out.size();
    out.resize(before + maxBytes);
    in.read(out.data() + before, static_cast<std::streamsize>(maxBytes));
    const auto got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
    out.resize(before + got);
    // eof (the file is shorter than asked) is normal; only a hard stream error is a failure.
    return !in.bad();
}

LogTailer::LogTailer(std::unique_ptr<IFileSource> source, Options options)
    : source_(std::move(source)), options_(options), attachAtEnd_(!options.fromStart) {}

TailerStats LogTailer::stats() const noexcept {
    return {offset_, totalBytes_, curiaBytes_, truncations_};
}

void LogTailer::onTruncation(PollResult& result) {
    std::vector<Event> events = pipeline_.truncateLog();
    result.events.insert(result.events.end(), std::make_move_iterator(events.begin()),
                         std::make_move_iterator(events.end()));
    offset_ = 0;
    totalBytes_ = 0;
    curiaBytes_ = 0;
}

bool LogTailer::readAppended(std::uint64_t size, PollResult& result) {
    std::string chunk;
    while (offset_ < size) {
        const std::size_t want =
            static_cast<std::size_t>(std::min<std::uint64_t>(kChunkBytes, size - offset_));
        chunk.clear();
        if (!source_->read(offset_, want, chunk)) {
            return false;
        }
        if (chunk.empty()) {
            return true;  // shrank since stat(); the next poll sees the truncation
        }
        if (chunk.size() > want) {
            chunk.resize(want);  // a source must not return more than asked
        }
        offset_ += chunk.size();
        totalBytes_ += chunk.size();
        result.bytesRead += chunk.size();
        const std::uint64_t curiaBefore = pipeline_.curiaLineBytes();
        std::vector<Event> events = pipeline_.feed(chunk);
        curiaBytes_ += pipeline_.curiaLineBytes() - curiaBefore;
        result.events.insert(result.events.end(), std::make_move_iterator(events.begin()),
                             std::make_move_iterator(events.end()));
    }
    return true;
}

PollResult LogTailer::poll() {
    PollResult result;
    const IFileSource::Stat stat = source_->stat();
    switch (stat.state) {
    case IFileSource::State::Missing:
        if (attached_) {
            // Deleted while attached: whatever was open is lost, like after a truncation.
            onTruncation(result);
            attached_ = false;
        }
        // A file that appears later is new: read it from its first byte.
        attachAtEnd_ = false;
        if (!missing_) {
            missing_ = true;
            result.notices.push_back(TailerNotice::Missing);
        }
        return result;
    case IFileSource::State::Error:
        if (!unreadable_) {
            unreadable_ = true;
            result.notices.push_back(TailerNotice::Unreadable);
        }
        return result;
    case IFileSource::State::Ok:
        break;
    }

    missing_ = false;
    if (!attached_) {
        offset_ = attachAtEnd_ ? stat.size : 0;
        attachAtEnd_ = false;
        attached_ = true;
        result.notices.push_back(TailerNotice::Attached);
    } else if (stat.size < offset_) {
        onTruncation(result);
        ++truncations_;
        result.notices.push_back(TailerNotice::Truncated);
    }
    // Recovery is judged by the read, not by the stat: a file that can be examined but not read
    // would otherwise flip between the two notices on every poll.
    const bool readable = readAppended(stat.size, result);
    if (!readable && !unreadable_) {
        unreadable_ = true;
        result.notices.push_back(TailerNotice::Unreadable);
    } else if (readable && unreadable_) {
        unreadable_ = false;
        result.notices.push_back(TailerNotice::Recovered);
    }
    return result;
}

}  // namespace curia::bridge
