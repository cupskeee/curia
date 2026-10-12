// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/redact.h"
#include "platform/env.h"

namespace curia {

// Development transcripts (CURIA_LOG_CONTENT=1, decision D19): a plain text file per question
// with the question, the answer, the usage and the timings, for the owner's quality review. They
// hold GAME TEXT (the answer quotes names from the realm), so they are never written inside a
// source tree, and never into the app log, which holds numbers only.
//
// Where they go: the folder named by CURIA_TRANSCRIPT_DIR, else <home>/curia_m4_results/
// transcripts (home = HOME, else USERPROFILE). A relative folder is taken from the current
// directory. The folder is REFUSED, with a message and nothing written, if it or any folder above
// it holds a ".git" entry (a checkout, a worktree or a submodule), after symbolic links on the way
// are resolved, so a transcript can never land in a repository's working tree.
//
// Rights: folders made here are 0700 and files 0600 on POSIX (a Windows profile is private
// already). Keys are scrubbed from every text (the registry's redact()); game text is not.
struct TranscriptDir {
    bool ok = false;
    std::filesystem::path path;
    // Why the folder was refused (set when !ok). Names the folder, never a secret.
    std::string refusal;
};

// Decides the folder (pure apart from reading the file system for ".git" entries and symbolic
// links). `configured` is CURIA_TRANSCRIPT_DIR. Does not create anything.
TranscriptDir resolveTranscriptDir(const std::optional<std::string>& configured,
                                   const platform::GetEnv& getenv);

// Creates the folder with owner-only rights (the missing levels), after checking again that it is
// not in a repository, and proves that a file can be created in it (a short-lived empty file that
// is removed again). Returns false with `error` set on failure; no transcript is written then.
bool prepareTranscriptDir(const std::filesystem::path& directory, std::string& error);

// One question and what came of it.
struct TranscriptRecord {
    std::string model;
    // The snapshot's number and game date text; snap 0 = no snapshot.
    std::int32_t snap = 0;
    std::string gameDate;
    std::uint64_t generation = 0;
    int personaVersion = 0;
    std::string question;
    std::string answer;
    // "complete", "stopped" or "failed".
    std::string end;
    // For a failed answer: the error code word and the plain summary.
    std::string errorCode;
    std::string errorSummary;
    int input = 0;
    int output = 0;
    int cacheRead = 0;
    int cacheWrite = 0;
    std::string stopReason;
    long long firstTokenMs = -1;
    long long totalMs = -1;
};

// The text of a transcript file (pure; the registry scrubs every text field).
std::string formatTranscript(const TranscriptRecord& record,
                             std::chrono::system_clock::time_point writtenAt,
                             const SecretRegistry* registry = nullptr);

// Writes one transcript file into the prepared folder, named
// transcript-<UTC time>-g<generation>-<random>.txt, never over an existing file. Returns the
// path, or nothing with `error` set.
std::optional<std::filesystem::path> writeTranscript(const std::filesystem::path& directory,
                                                     const TranscriptRecord& record,
                                                     std::string& error,
                                                     const SecretRegistry* registry = nullptr);

}  // namespace curia
