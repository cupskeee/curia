// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Which game log the chat window reads. Pure: the file system is asked through an injected
// function, so the tests need no real folders.
namespace curia::chatview {

using ExistsFn = std::function<bool(const std::filesystem::path&)>;

struct LogPathChoice {
    // The file to read; empty = none was found (the window says CK3's folder was not found and
    // reads nothing).
    std::filesystem::path path;
    // It came from --log.
    bool fromFlag = false;
};

// <userDir>/logs/debug.log
std::filesystem::path logFileIn(const std::filesystem::path& userDir);

// --log <path> if given (used as it is, even if the file does not exist yet), else the first
// logs/debug.log under the candidate user folders (Platform::candidateCk3UserDirs(), most likely
// first) that exists, else none.
//
// How the reader treats the file (bridge/log_tailer.h): a file that exists when the window opens is
// attached at its END, so the window shows nothing about the realm until the player clicks Advisor
// after Curia started (the "nothing read yet" text says so). A --log file that does not exist yet
// is reported missing once and read from its first byte when it appears; CK3 empties its log at
// launch, which the reader sees as a truncation and follows.
LogPathChoice chooseLogPath(const std::string& flagPath,
                            const std::vector<std::filesystem::path>& userDirs,
                            const ExistsFn& exists);

}  // namespace curia::chatview
