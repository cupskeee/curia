// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>

#include "bridge_test_support.h"
#include "core/transcript_writer.h"

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using curia::TranscriptRecord;
using curia::bridge::test::TempDir;

namespace {

const std::string kKey = std::string("sk-ant-") + "FAKE" + "0123456789abcdef0123";

curia::platform::GetEnv envOf(std::map<std::string, std::string> values) {
    return [values = std::move(values)](const char* name) {
        const auto found = values.find(name);
        return found != values.end() ? found->second : std::string();
    };
}

std::string readAll(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

void makeGit(const fs::path& directory) {
    fs::create_directories(directory / ".git");
}

TranscriptRecord sampleRecord() {
    TranscriptRecord record;
    record.model = "mock-model-1";
    record.snap = 12;
    record.gameDate = "15 September 1066";
    record.generation = 3;
    record.personaVersion = 2;
    record.question = "is my heir safe?";
    record.answer = "Your heir \"Test Heir Two\" likes you.";
    record.end = "complete";
    record.input = 100;
    record.output = 20;
    record.cacheRead = 80;
    record.cacheWrite = 5;
    record.stopReason = "end_turn";
    record.firstTokenMs = 812;
    record.totalMs = 3000;
    return record;
}

}  // namespace

TEST_CASE("transcript folder: the default is under the home folder") {
    const TempDir home;
    const curia::TranscriptDir dir =
        curia::resolveTranscriptDir(std::nullopt, envOf({{"HOME", home.path("h")}}));
    REQUIRE(dir.ok);
    CHECK(dir.path == fs::absolute(fs::path(home.path("h")) / "curia_m4_results" / "transcripts"));
    // USERPROFILE is used when HOME is not set.
    const curia::TranscriptDir viaProfile =
        curia::resolveTranscriptDir(std::nullopt, envOf({{"USERPROFILE", home.path("p")}}));
    REQUIRE(viaProfile.ok);
    CHECK(viaProfile.path.string().find("curia_m4_results") != std::string::npos);
    // HOME wins over USERPROFILE.
    const curia::TranscriptDir both = curia::resolveTranscriptDir(
        std::nullopt, envOf({{"HOME", home.path("h")}, {"USERPROFILE", home.path("p")}}));
    CHECK(both.path == fs::absolute(fs::path(home.path("h")) / "curia_m4_results" / "transcripts"));
}

TEST_CASE("transcript folder: no home and no setting is refused") {
    const curia::TranscriptDir dir = curia::resolveTranscriptDir(std::nullopt, envOf({}));
    CHECK_FALSE(dir.ok);
    CHECK_FALSE(dir.refusal.empty());
}

TEST_CASE("transcript folder: a configured folder wins, a relative one is made absolute") {
    const TempDir temp;
    const std::string chosen = temp.path("chosen");
    const curia::TranscriptDir dir =
        curia::resolveTranscriptDir(chosen, envOf({{"HOME", temp.path("h")}}));
    REQUIRE(dir.ok);
    CHECK(dir.path == fs::path(chosen));
    const curia::TranscriptDir relative =
        curia::resolveTranscriptDir(std::string("some_relative_folder"), envOf({}));
    // Refused or accepted depending on where the tests run (a checkout has .git above the
    // working directory), but never relative.
    CHECK((relative.ok ? relative.path.is_absolute() : !relative.refusal.empty()));
}

TEST_CASE("transcript folder: refused when it or an ancestor has a .git entry") {
    const TempDir temp;
    const fs::path repo = fs::path(temp.path("repo"));
    makeGit(repo);
    const fs::path inside = repo / "docs" / "notes";
    for (const fs::path& candidate : {repo, repo / "docs", inside}) {
        const curia::TranscriptDir dir = curia::resolveTranscriptDir(candidate.string(), envOf({}));
        CHECK_FALSE(dir.ok);
        CHECK(dir.refusal.find(".git") != std::string::npos);
    }
    // A .git FILE (a worktree or a submodule) counts as well.
    const fs::path worktree = fs::path(temp.path("wt"));
    fs::create_directories(worktree);
    std::ofstream(worktree / ".git") << "gitdir: elsewhere\n";
    CHECK_FALSE(curia::resolveTranscriptDir((worktree / "t").string(), envOf({})).ok);
    // The default folder under a home that is itself a repository is refused too.
    makeGit(fs::path(temp.path("home")));
    CHECK_FALSE(curia::resolveTranscriptDir(std::nullopt, envOf({{"HOME", temp.path("home")}})).ok);
    // A sibling of a repository is fine.
    CHECK(curia::resolveTranscriptDir(temp.path("other"), envOf({})).ok);
}

#if !defined(_WIN32)
TEST_CASE("transcript folder: a symbolic link into a repository is followed and refused") {
    const TempDir temp;
    const fs::path repo = fs::path(temp.path("repo"));
    makeGit(repo);
    const fs::path link = fs::path(temp.path("link"));
    std::error_code ec;
    fs::create_directory_symlink(repo, link, ec);
    REQUIRE_FALSE(ec);
    CHECK_FALSE(curia::resolveTranscriptDir((link / "t").string(), envOf({})).ok);
    std::string error;
    CHECK_FALSE(curia::prepareTranscriptDir(link / "t", error));
    CHECK_FALSE(fs::exists(repo / "t"));
}
#endif

TEST_CASE("transcript folder: prepare creates every missing level and refuses a repository") {
    const TempDir temp;
    const fs::path target = fs::path(temp.path("a")) / "b" / "c";
    std::string error;
    REQUIRE(curia::prepareTranscriptDir(target, error));
    CHECK(fs::is_directory(target));
#if !defined(_WIN32)
    struct stat info {};
    REQUIRE(::stat(target.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
    REQUIRE(::stat((fs::path(temp.path("a")) / "b").c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0700);
#endif
    // Preparing again is fine and changes nothing.
    CHECK(curia::prepareTranscriptDir(target, error));

    const fs::path repo = fs::path(temp.path("repo"));
    makeGit(repo);
    CHECK_FALSE(curia::prepareTranscriptDir(repo / "transcripts", error));
    CHECK(error.find(".git") != std::string::npos);
    CHECK_FALSE(fs::exists(repo / "transcripts"));
}

TEST_CASE("transcript: the text has the question, the answer, the usage and the timings") {
    const std::string text = curia::formatTranscript(
        sampleRecord(), std::chrono::system_clock::time_point{} + std::chrono::hours(24));
    CHECK(text.find("written: 1970-01-02T00:00:00.000Z\n") != std::string::npos);
    CHECK(text.find("model: mock-model-1\n") != std::string::npos);
    CHECK(text.find("snapshot: 12\n") != std::string::npos);
    CHECK(text.find("game_date: 15 September 1066\n") != std::string::npos);
    CHECK(text.find("first_token_ms: 812\n") != std::string::npos);
    CHECK(text.find("total_ms: 3000\n") != std::string::npos);
    CHECK(text.find("usage: input=100 output=20 cache_read=80 cache_write=5\n") !=
          std::string::npos);
    CHECK(text.find("--- question ---\nis my heir safe?\n") != std::string::npos);
    CHECK(text.find("--- answer ---\nYour heir \"Test Heir Two\" likes you.\n") !=
          std::string::npos);
    CHECK(text.find("error:") == std::string::npos);
    // The settings sit next to the model and the persona version; "default" when not sent.
    CHECK(text.find("persona_version: 2\neffort: default\nthinking: default\n") !=
          std::string::npos);
    TranscriptRecord tuned = sampleRecord();
    tuned.effort = "low";
    tuned.thinking = "between_tools";
    const std::string tunedText = curia::formatTranscript(tuned, std::chrono::system_clock::now());
    CHECK(tunedText.find("effort: low\nthinking: between_tools\n") != std::string::npos);
}

TEST_CASE("transcript: game text is kept, keys are scrubbed everywhere") {
    curia::SecretRegistry registry;
    REQUIRE(registry.add(kKey));
    TranscriptRecord record = sampleRecord();
    record.question = "my key is " + kKey;
    record.answer = "echo " + kKey + " and Test Vassal Three";
    record.model = "m-" + kKey;
    record.errorCode = "Network";
    record.errorSummary = "broke " + kKey;
    const std::string text =
        curia::formatTranscript(record, std::chrono::system_clock::now(), &registry);
    CHECK(text.find("0123456789abcdef") == std::string::npos);
    CHECK(text.find("Test Vassal Three") != std::string::npos);
    CHECK(text.find("error: Network: broke [redacted]\n") != std::string::npos);
}

TEST_CASE("transcript: files are written once, with owner-only rights") {
    const TempDir temp;
    const fs::path dir = fs::path(temp.path("t"));
    std::string error;
    REQUIRE(curia::prepareTranscriptDir(dir, error));
    const std::optional<fs::path> first = curia::writeTranscript(dir, sampleRecord(), error);
    const std::optional<fs::path> second = curia::writeTranscript(dir, sampleRecord(), error);
    const fs::path firstPath = curia::bridge::test::must(first);
    const fs::path secondPath = curia::bridge::test::must(second);
    CHECK(firstPath != secondPath);
    CHECK(firstPath.parent_path() == dir);
    CHECK(firstPath.filename().string().find("transcript-") == 0);
    CHECK(firstPath.filename().string().find("-g3-") != std::string::npos);
    CHECK(readAll(firstPath).find("--- answer ---") != std::string::npos);
#if !defined(_WIN32)
    struct stat info {};
    REQUIRE(::stat(firstPath.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
#endif
}

TEST_CASE("transcript: nothing is written into a repository") {
    const TempDir temp;
    const fs::path repo = fs::path(temp.path("repo"));
    makeGit(repo);
    fs::create_directories(repo / "t");
    std::string error;
    CHECK_FALSE(curia::writeTranscript(repo / "t", sampleRecord(), error).has_value());
    CHECK(error.find(".git") != std::string::npos);
    CHECK(fs::is_empty(repo / "t"));
}
