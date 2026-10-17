// SPDX-License-Identifier: MIT
#include "core/transcript_writer.h"

#include <cstdio>
#include <random>
#include <ranges>
#include <system_error>
#include <utility>

#include "core/app_log.h"

#if defined(_WIN32)
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace curia {

namespace {

namespace fs = std::filesystem;

constexpr const char* kDefaultFolderA = "curia_m4_results";
constexpr const char* kDefaultFolderB = "transcripts";

// Empty when the folder is fine, else the reason.
std::string repositoryProblem(const fs::path& directory) {
    std::error_code ec;
    // Symbolic links on the way are resolved, so a link into a checkout is seen as the checkout.
    const fs::path resolved = fs::weakly_canonical(fs::absolute(directory, ec), ec);
    if (ec) {
        return "the transcript folder cannot be resolved";
    }
    for (fs::path folder = resolved;; folder = folder.parent_path()) {
        std::error_code existsError;
        if (fs::exists(folder / ".git", existsError)) {
            return "the transcript folder is inside a source tree (" + folder.string() +
                   " has a .git entry); transcripts hold game text and are never written there. "
                   "Choose a folder outside every repository";
        }
        if (!folder.has_parent_path() || folder == folder.parent_path()) {
            break;
        }
    }
    return {};
}

std::string compactTimestamp(std::chrono::system_clock::time_point time) {
    const std::string text = AppLog::formatTimestamp(time);
    std::string out;
    for (const char ch : text) {
        if (ch != '-' && ch != ':' && ch != '.') {
            out.push_back(ch);
        }
    }
    return out;
}

std::string randomSuffix() {
    std::random_device device;
    std::uniform_int_distribution<int> digit(0, 15);
    std::string out;
    for (int i = 0; i < 6; ++i) {
        out.push_back("0123456789abcdef"[digit(device)]);
    }
    return out;
}

// A new file that must not exist yet, owner-only on POSIX.
std::FILE* createNew(const fs::path& path) {
#if defined(_WIN32)
    std::error_code ec;
    if (fs::exists(path, ec)) {
        return nullptr;
    }
    // _SH_DENYNO: the owner may read the file while the app is still running.
    return _wfsopen(path.c_str(), L"wb", _SH_DENYNO);
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) {
        return nullptr;
    }
    std::FILE* file = ::fdopen(fd, "wb");
    if (file == nullptr) {
        ::close(fd);
    }
    return file;
#endif
}

std::string scrubbed(const std::string& text, const SecretRegistry* registry) {
    return registry != nullptr ? registry->redact(text) : redact(text);
}

void line(std::string& out, const std::string& key, const std::string& value) {
    out += key;
    out += ": ";
    out += value;
    out += '\n';
}

}  // namespace

TranscriptDir resolveTranscriptDir(const std::optional<std::string>& configured,
                                   const platform::GetEnv& getenv) {
    TranscriptDir result;
    fs::path directory;
    if (configured.has_value() && !configured->empty()) {
        directory = fs::path(*configured);
    } else {
        std::string home = getenv ? getenv("HOME") : std::string();
        if (home.empty() && getenv) {
            home = getenv("USERPROFILE");
        }
        if (home.empty()) {
            result.refusal = "there is no home folder to put transcripts in; name a folder with "
                             "the transcript-folder setting";
            return result;
        }
        directory = fs::path(home) / kDefaultFolderA / kDefaultFolderB;
    }
    std::error_code ec;
    directory = fs::absolute(directory, ec);
    if (ec) {
        result.refusal = "the transcript folder cannot be resolved";
        return result;
    }
    const std::string problem = repositoryProblem(directory);
    if (!problem.empty()) {
        result.refusal = problem;
        return result;
    }
    result.ok = true;
    result.path = directory;
    return result;
}

bool prepareTranscriptDir(const fs::path& directory, std::string& error) {
    const std::string problem = repositoryProblem(directory);
    if (!problem.empty()) {
        error = problem;
        return false;
    }
    // The levels that do not exist yet, outermost first; each gets owner-only rights.
    std::vector<fs::path> missing;
    std::error_code ec;
    for (fs::path folder = directory; !folder.empty(); folder = folder.parent_path()) {
        if (fs::exists(folder, ec)) {
            break;
        }
        missing.push_back(folder);
        if (!folder.has_parent_path() || folder == folder.parent_path()) {
            break;
        }
    }
    for (const fs::path& level : std::views::reverse(missing)) {
        std::error_code createError;
        fs::create_directory(level, createError);
        if (createError) {
            error = "the transcript folder cannot be created";
            return false;
        }
        std::error_code permError;
        fs::permissions(level, fs::perms::owner_all, fs::perm_options::replace, permError);
    }
    std::error_code dirError;
    if (!fs::is_directory(directory, dirError)) {
        error = "the transcript folder is not a folder";
        return false;
    }
    // A link that appeared meanwhile is checked once more.
    const std::string again = repositoryProblem(directory);
    if (!again.empty()) {
        error = again;
        return false;
    }
    // Writable? Found out now, not after the first (paid) answer.
    for (int attempt = 0; attempt < 5; ++attempt) {
        const fs::path probe = directory / (".curia-write-test-" + randomSuffix());
        std::FILE* file = createNew(probe);
        if (file == nullptr) {
            continue;
        }
        std::fclose(file);
        std::error_code removeError;
        fs::remove(probe, removeError);
        return true;
    }
    error = "the transcript folder cannot be written to";
    return false;
}

std::string formatTranscript(const TranscriptRecord& record,
                             std::chrono::system_clock::time_point writtenAt,
                             const SecretRegistry* registry) {
    std::string out = "Curia transcript (development only; it holds game text)\n";
    line(out, "written", AppLog::formatTimestamp(writtenAt));
    line(out, "model", scrubbed(record.model, registry));
    line(out, "snapshot", record.snap > 0 ? std::to_string(record.snap) : std::string("none"));
    line(out, "game_date", scrubbed(record.gameDate, registry));
    line(out, "generation", std::to_string(record.generation));
    line(out, "persona_version", std::to_string(record.personaVersion));
    line(out, "effort", scrubbed(record.effort, registry));
    line(out, "thinking", scrubbed(record.thinking, registry));
    line(out, "end", scrubbed(record.end, registry));
    if (!record.errorCode.empty() || !record.errorSummary.empty()) {
        line(out, "error",
             scrubbed(record.errorCode, registry) + ": " + scrubbed(record.errorSummary, registry));
    }
    line(out, "first_token_ms", std::to_string(record.firstTokenMs));
    line(out, "total_ms", std::to_string(record.totalMs));
    line(out, "usage",
         "input=" + std::to_string(record.input) + " output=" + std::to_string(record.output) +
             " cache_read=" + std::to_string(record.cacheRead) +
             " cache_write=" + std::to_string(record.cacheWrite));
    line(out, "stop_reason", scrubbed(record.stopReason, registry));
    out += "\n--- question ---\n";
    out += scrubbed(record.question, registry);
    out += "\n\n--- answer ---\n";
    out += scrubbed(record.answer, registry);
    out += '\n';
    return out;
}

std::optional<fs::path> writeTranscript(const fs::path& directory, const TranscriptRecord& record,
                                        std::string& error, const SecretRegistry* registry) {
    const std::string problem = repositoryProblem(directory);
    if (!problem.empty()) {
        error = problem;
        return std::nullopt;
    }
    const auto now = std::chrono::system_clock::now();
    const std::string text = formatTranscript(record, now, registry);
    for (int attempt = 0; attempt < 5; ++attempt) {
        fs::path path =
            directory / ("transcript-" + compactTimestamp(now) + "-g" +
                         std::to_string(record.generation) + "-" + randomSuffix() + ".txt");
        std::FILE* file = createNew(path);
        if (file == nullptr) {
            continue;
        }
        const std::size_t written = std::fwrite(text.data(), 1, text.size(), file);
        const bool closed = std::fclose(file) == 0;
        if (written != text.size() || !closed) {
            error = "the transcript could not be written";
            return std::nullopt;
        }
        std::error_code ec;
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, ec);
        return path;
    }
    error = "the transcript file could not be created";
    return std::nullopt;
}

}  // namespace curia
