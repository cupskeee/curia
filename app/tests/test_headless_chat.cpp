// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include <nlohmann/json.hpp>

#include "advisor/persona.h"
#include "advisor_test_support.h"
#include "bridge_test_support.h"
#include "core/headless_chat.h"
#include "core/redact.h"

namespace fs = std::filesystem;
using curia::HeadlessChatConfig;
using curia::HeadlessChatResult;
using curia::advisor::test::FakeProvider;
using curia::bridge::test::TempDir;
using nlohmann::json;

namespace {

const std::string kKey = std::string("sk-ant-") + "FAKE" + "headless0123456789ab";

struct Run {
    HeadlessChatResult result;
    std::vector<json> events;
    std::string out;
    std::string err;

    std::vector<json> of(const std::string& name) const {
        std::vector<json> found;
        for (const json& event : events) {
            if (event.at("event") == name) {
                found.push_back(event);
            }
        }
        return found;
    }
    // The names of all events, in order.
    std::vector<std::string> names() const {
        std::vector<std::string> found;
        for (const json& event : events) {
            found.push_back(event.at("event").get<std::string>());
        }
        return found;
    }
    std::size_t terminals() const {
        return of("done").size() + of("cancelled").size() + of("error").size();
    }
};

struct Rig {
    TempDir dir;
    curia::SecretRegistry registry;
    std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
    HeadlessChatConfig config;
    std::string log;

    explicit Rig(int frames = 3) {
        log = dir.path("debug.log");
        std::string bytes;
        for (int snap = 1; snap <= frames; ++snap) {
            bytes += curia::bridge::test::stream(curia::bridge::test::exampleFrame(snap));
        }
        REQUIRE(curia::bridge::test::writeFile(log, bytes));
        config.options.headlessChat = true;
        config.options.logPath = log;
        config.options.fromStart = true;
#if defined(CURIA_ENABLE_DEV_ENV)
        config.options.tickMs = 10;  // a test-only option: refused by a build without the dev setup
#endif
        config.options.timeoutSeconds = 30;
        config.model = "mock-model-1";
        config.provider = provider;
        config.registry = &registry;
        config.pollInterval = std::chrono::milliseconds(10);
        config.getenv = [](const char*) { return std::string(); };
        REQUIRE(registry.add(kKey));
        FakeProvider::Script script;
        script.tokens = {"Check ", "your heir."};
        script.usage.input = 500;
        script.usage.output = 12;
        script.usage.cacheRead = 400;
        script.usage.stopReason = "end_turn";
        script.onThread = true;
        provider->setScript(script);
    }

    Run run(const std::string& input) {
        std::istringstream in(input);
        std::ostringstream out;
        std::ostringstream err;
        Run result;
        result.result = curia::runHeadlessChat(config, in, out, err);
        result.out = out.str();
        result.err = err.str();
        std::istringstream lines(result.out);
        std::string line;
        while (std::getline(lines, line)) {
            result.events.push_back(json::parse(line));
        }
        return result;
    }
};

std::size_t indexOf(const std::vector<std::string>& names, const std::string& name) {
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == name) {
            return i;
        }
    }
    return names.size();
}

}  // namespace

TEST_CASE("headless chat: the newest snapshot, a question, a streamed answer") {
    Rig rig;
    const Run run = rig.run("ask is my heir safe?\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    REQUIRE(run.of("ready").size() == 1);
    const json ready = run.of("ready")[0];
    CHECK(ready["snapshot"] == 3);
    CHECK(ready["frames"] == 3);
    CHECK(ready["from_start"] == true);
    CHECK(ready["model"] == "mock-model-1");
    CHECK(ready["effort"] == "default");
    CHECK(ready["thinking"] == "default");
    CHECK(ready["persona_version"] == curia::advisor::kPersonaVersion);
    CHECK(ready["transcripts"].is_null());
    // Catching up with the log does not report every old frame: one snapshot, then ready.
    CHECK(run.of("snapshot").size() == 1);
    CHECK(run.of("overlay_open").empty());
    const std::vector<std::string> names = run.names();
    CHECK(indexOf(names, "snapshot") < indexOf(names, "ready"));
    CHECK(indexOf(names, "ready") < indexOf(names, "ask"));
    CHECK(indexOf(names, "ask") < indexOf(names, "token"));
    CHECK(indexOf(names, "token") < indexOf(names, "done"));
    REQUIRE(run.of("done").size() == 1);
    const json done = run.of("done")[0];
    CHECK(done["text"] == "Check your heir.");
    CHECK(done["usage"]["input"] == 500);
    CHECK(done["usage"]["cache_read"] == 400);
    CHECK(done["stop_reason"] == "end_turn");
    CHECK(done["generation"] == 1);
    CHECK(done["max_tokens"] == false);
    CHECK(done["total_ms"].is_number());
    CHECK(done["effort"] == "default");
    CHECK(done["thinking"] == "default");
    CHECK_FALSE(done.contains("thinking_started_ms"));  // no thinking block came
    CHECK(run.of("status").size() == 2);                // waiting, streaming; no "thinking"
    std::string joined;
    for (const json& token : run.of("token")) {
        joined += token["text"].get<std::string>();
    }
    CHECK(joined == "Check your heir.");
    CHECK(run.terminals() == 1);
    // The request carried the newest snapshot.
    REQUIRE(rig.provider->callCount() == 1);
    CHECK(rig.provider->call(0).request.snapshotText.find("Test Heir Two") != std::string::npos);
    CHECK(run.of("ask")[0]["text"] == "is my heir safe?");
}

TEST_CASE("headless chat: the effort and thinking settings reach the request and the events") {
    Rig rig;
    rig.config.options.effort = curia::providers::Effort::Low;
    rig.config.options.thinking = curia::providers::Thinking::BetweenTools;
    FakeProvider::Script script;
    script.tokens = {"Think ", "less."};
    script.onThread = true;
    script.thinkingFirst = true;
    script.gap = std::chrono::milliseconds(20);
    rig.provider->setScript(script);
    const Run run = rig.run("ask one\nask two\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    REQUIRE(rig.provider->callCount() == 2);
    CHECK((rig.provider->call(0).request.effort == curia::providers::Effort::Low));
    CHECK((rig.provider->call(0).request.thinking == curia::providers::Thinking::BetweenTools));
    REQUIRE(run.of("ready").size() == 1);
    CHECK(run.of("ready")[0]["effort"] == "low");
    CHECK(run.of("ready")[0]["thinking"] == "between_tools");
    REQUIRE(run.of("done").size() == 2);
    for (const json& done : run.of("done")) {
        CHECK(done["effort"] == "low");
        CHECK(done["thinking"] == "between_tools");
        REQUIRE(done.contains("thinking_started_ms"));
        CHECK(done["thinking_started_ms"].get<long long>() >= 0);
        CHECK(done["thinking_started_ms"].get<long long>() <=
              done["first_token_ms"].get<long long>());
    }
    // "thinking" once per answer, after "waiting" and before the answer's first token.
    const std::vector<std::string> names = run.names();
    std::vector<std::string> states;
    for (const json& status : run.of("status")) {
        states.push_back(status["state"].get<std::string>());
    }
    CHECK((states == std::vector<std::string>{"waiting", "thinking", "streaming", "waiting",
                                              "thinking", "streaming"}));
    std::size_t thinkingAt = names.size();
    for (std::size_t i = 0; i < run.events.size(); ++i) {
        if (run.events[i]["event"] == "status" && run.events[i]["state"] == "thinking") {
            thinkingAt = i;
            break;
        }
    }
    CHECK(thinkingAt < indexOf(names, "token"));
}

TEST_CASE("headless chat: --frame selects a snapshot, an unknown number ends the run") {
    {
        Rig rig;
        rig.config.options.frameSnap = 2;
        const Run run = rig.run("");
        CHECK(run.result.exitCode == curia::kHeadlessExitOk);
        REQUIRE(run.of("ready").size() == 1);
        CHECK(run.of("ready")[0]["snapshot"] == 2);
    }
    {
        Rig rig;
        rig.config.options.frameSnap = 99;
        const Run run = rig.run("ask hello\n");
        CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
        REQUIRE(run.of("fatal").size() == 1);
        CHECK(run.of("fatal")[0]["code"] == "snapshot_not_found");
        CHECK(rig.provider->callCount() == 0);
        CHECK(run.of("ready").empty());
    }
}

TEST_CASE("headless chat: --frame ignores the damage of frames with other numbers") {
    Rig rig(0);
    std::vector<std::string> broken = curia::bridge::test::exampleFrame(8);
    REQUIRE(broken.size() > 2);
    broken.pop_back();  // no END record: the frame is damaged when the next one begins
    const std::string bytes = curia::bridge::test::stream(curia::bridge::test::exampleFrame(7)) +
                              curia::bridge::test::stream(broken) +
                              curia::bridge::test::stream(curia::bridge::test::exampleFrame(9));
    REQUIRE(curia::bridge::test::writeFile(rig.log, bytes));
    {
        rig.config.options.frameSnap = 7;
        const Run run = rig.run("");
        CHECK(run.result.exitCode == curia::kHeadlessExitOk);
        REQUIRE(run.of("ready").size() == 1);
        CHECK(run.of("ready")[0]["snapshot"] == 7);
        REQUIRE(run.of("snapshot").size() == 1);
        CHECK(run.of("snapshot")[0]["complete"] == true);
        CHECK(run.of("snapshot")[0]["unreadable"] == false);
    }
    {
        // Without --frame the newest frame is complete too, and the damaged one in between is
        // replaced by it.
        rig.config.options.frameSnap.reset();
        const Run run = rig.run("");
        REQUIRE(run.of("ready").size() == 1);
        CHECK(run.of("ready")[0]["snapshot"] == 9);
    }
    {
        // The damaged frame itself can be asked for: it is reported as unreadable.
        rig.config.options.frameSnap = 8;
        const Run run = rig.run("");
        CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
        REQUIRE(run.of("fatal").size() == 1);
        CHECK(run.of("fatal")[0]["code"] == "snapshot_not_found");
    }
}

TEST_CASE("headless chat: a log path that is a folder is a setup error") {
    Rig rig;
    fs::create_directories(rig.dir.path("a_folder"));
    rig.config.options.logPath = rig.dir.path("a_folder");
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
    REQUIRE(run.of("fatal").size() == 1);
    CHECK(run.of("fatal")[0]["code"] == "setup");
    CHECK(rig.provider->callCount() == 0);
}

TEST_CASE("headless chat: not configured sends nothing and names the variables only") {
    Rig rig;
    rig.config.provider = nullptr;
    rig.config.missing = {"SOME_KEY_VARIABLE", "SOME_MODEL_VARIABLE"};
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
    REQUIRE(run.of("not_configured").size() == 1);
    const json event = run.of("not_configured")[0];
    CHECK(event["missing"] == json::array({"SOME_KEY_VARIABLE", "SOME_MODEL_VARIABLE"}));
    CHECK(run.of("ready").empty());
    CHECK(rig.provider->callCount() == 0);
    CHECK(run.out.find("FAKE") == std::string::npos);
}

TEST_CASE("headless chat: a missing log file with --from-start is a setup error") {
    Rig rig;
    rig.config.options.logPath = rig.dir.path("nope.log");
    const Run run = rig.run("");
    CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
    REQUIRE(run.of("fatal").size() == 1);
    CHECK(run.of("fatal")[0]["code"] == "setup");
}

TEST_CASE("headless chat: questions run one after the other, in order") {
    Rig rig;
    const Run run = rig.run("ask first\nask second\nask third\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    REQUIRE(run.of("done").size() == 3);
    CHECK(run.of("done")[0]["generation"] == 1);
    CHECK(run.of("done")[2]["generation"] == 3);
    CHECK(rig.provider->callCount() == 3);
    // Each ask event comes after the previous question's terminal event.
    const std::vector<std::string> names = run.names();
    std::size_t asks = 0;
    std::size_t terminalsSeen = 0;
    for (const std::string& name : names) {
        if (name == "ask") {
            CHECK(terminalsSeen == asks);
            ++asks;
        } else if (name == "done") {
            ++terminalsSeen;
        }
    }
    // Later questions carry the earlier ones as history.
    CHECK(rig.provider->call(2).request.conversation.size() == 5);
}

TEST_CASE("headless chat: cancel ends the question with exactly one terminal event") {
    Rig rig;
    FakeProvider::Script slow;
    for (int i = 0; i < 40; ++i) {
        slow.tokens.push_back("word" + std::to_string(i) + " ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(50);
    rig.provider->setScript(slow);
    const Run run = rig.run("ask tell me everything\ncancel\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    CHECK(run.terminals() == 1);
    REQUIRE(run.of("cancelled").size() == 1);
    CHECK(run.of("done").empty());
    CHECK(run.of("error").empty());
    const json cancelled = run.of("cancelled")[0];
    CHECK(cancelled["text"].get<std::string>().size() < 40 * 7);
}

TEST_CASE("headless chat: cancel and quit act at once, even behind a queued question") {
    Rig rig;
    FakeProvider::Script slow;
    for (int i = 0; i < 200; ++i) {
        slow.tokens.push_back("word ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(50);
    rig.provider->setScript(slow);
    SUBCASE("quit behind a waiting ask") {
        const Run run = rig.run("ask one\nask two\nquit\n");
        CHECK(run.result.exitCode == curia::kHeadlessExitOk);
        CHECK(rig.provider->callCount() == 1);
        CHECK(run.of("cancelled").size() == 1);
        CHECK(run.terminals() == 1);
    }
    SUBCASE("cancel behind a waiting ask stops the first, then the second runs") {
        const Run run = rig.run("ask one\nask two\ncancel\nquit\n");
        CHECK(run.result.exitCode == curia::kHeadlessExitOk);
        CHECK(rig.provider->callCount() == 2);
        CHECK(run.of("cancelled").size() == 2);
        CHECK(run.terminals() == 2);
        CHECK(run.of("ask")[0]["text"] == "one");
        CHECK(run.of("ask")[1]["text"] == "two");
    }
    SUBCASE("arm and ck3 behind a waiting ask are not held up") {
        const Run run = rig.run("ask one\nask two\narm\nck3 stopped\nquit\n");
        CHECK(run.of("overlay_open").size() == 1);
        CHECK(run.of("ck3").size() == 1);
        CHECK(rig.provider->callCount() == 1);
    }
}

TEST_CASE("headless chat: cancel with nothing in flight is reported, quit cancels what runs") {
    Rig rig;
    FakeProvider::Script slow;
    for (int i = 0; i < 40; ++i) {
        slow.tokens.push_back("word ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(50);
    rig.provider->setScript(slow);
    const Run run = rig.run("cancel\nask go\nquit\nask never\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    CHECK(run.of("cancel_ignored").size() == 1);
    CHECK(run.of("cancelled").size() == 1);
    CHECK(rig.provider->callCount() == 1);
    CHECK(run.terminals() == 1);
}

TEST_CASE("headless chat: an error keeps the partial text and exits 1") {
    Rig rig;
    FakeProvider::Script broken;
    broken.tokens = {"Half an ", "answer"};
    curia::providers::ChatError error;
    error.code = curia::providers::ChatError::Code::Overloaded;
    error.message = "mock overloaded";
    error.retryable = true;
    error.httpStatus = 529;
    error.partialText = "Half an answer";
    broken.error = error;
    broken.onThread = true;
    rig.provider->setScript(broken);
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitFailed);
    CHECK(run.terminals() == 1);
    REQUIRE(run.of("error").size() == 1);
    const json event = run.of("error")[0];
    CHECK(event["code"] == "Overloaded");
    CHECK(event["retryable"] == true);
    CHECK(event["http_status"] == 529);
    CHECK(event["partial_text"] == "Half an answer");
    CHECK(event["partial_len"] == 14);
    CHECK(event["message"].get<std::string>().find("busy") != std::string::npos);
}

TEST_CASE(
    "headless chat: a key split over two tokens is redacted in the tokens and the final text") {
    Rig rig;
    FakeProvider::Script script;
    script.tokens = {"The key is " + kKey.substr(0, 14), kKey.substr(14) + " and that is all."};
    script.onThread = true;
    rig.provider->setScript(script);
    const Run run = rig.run("ask repeat the key\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    CHECK(run.out.find("headless0123456789ab") == std::string::npos);
    CHECK(run.out.find(kKey.substr(0, 14)) == std::string::npos);
    CHECK(run.err.find("FAKE") == std::string::npos);
    REQUIRE(run.of("done").size() == 1);
    CHECK(run.of("done")[0]["text"] == "The key is [redacted] and that is all.");
    std::string joined;
    for (const json& event : run.events) {
        if (event["event"] == "token") {
            joined += event["text"].get<std::string>();
        } else if (event["event"] == "text_reset") {
            joined = event["text"].get<std::string>();
        }
    }
    CHECK(joined == "The key is [redacted] and that is all.");
}

TEST_CASE("headless chat: a question holding the key is redacted before it is sent or printed") {
    Rig rig;
    const Run run = rig.run("ask is " + kKey + " safe?\n");
    CHECK(run.out.find("headless0123456789ab") == std::string::npos);
    REQUIRE(rig.provider->callCount() == 1);
    const auto& conversation = rig.provider->call(0).request.conversation;
    REQUIRE_FALSE(conversation.empty());
    CHECK(conversation.back().text.find("headless0123456789ab") == std::string::npos);
}

TEST_CASE("headless chat: bad lines are reported and do not stop the run") {
    Rig rig;
    const Run run = rig.run("\ndance\nask\nask real question\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    CHECK(run.of("bad_command").size() == 2);
    CHECK(run.of("done").size() == 1);
}

TEST_CASE("headless chat: retry answers the last question again") {
    Rig rig;
    const Run run = rig.run("retry\nask once\nretry\n");
    CHECK(run.of("ask_refused").size() == 1);
    CHECK(run.of("ask_refused")[0]["outcome"] == "NothingToRetry");
    CHECK(run.of("done").size() == 2);
}

TEST_CASE("headless chat: the end of the input waits for the answer in flight") {
    Rig rig;
    FakeProvider::Script slow;
    slow.tokens = {"a ", "b ", "c ", "d ", "e "};
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(40);
    rig.provider->setScript(slow);
    const Run run = rig.run("ask hello\n");  // EOF right after
    CHECK(run.of("done").size() == 1);
    CHECK(run.of("done")[0]["text"] == "a b c d e ");
    CHECK_FALSE(run.result.inputStillBlocked);
}

#if defined(CURIA_ENABLE_DEV_ENV)
TEST_CASE("headless chat: the notices of the silence detectors") {
    Rig rig;
    rig.config.options.armedWaitMs = 100;
    rig.config.options.armSnapshotAgeMs = 0;
    FakeProvider::Script slow;
    for (int i = 0; i < 12; ++i) {
        slow.tokens.push_back("w ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(60);
    rig.provider->setScript(slow);

    SUBCASE("armed with a running game: restart_ck3 after the short threshold") {
        const Run run = rig.run("arm\nask keep the run alive\n");
        const std::vector<json> notices = run.of("notice");
        REQUIRE_FALSE(notices.empty());
        CHECK(notices.back()["kind"] == "restart_ck3");
        CHECK(run.of("overlay_open").size() == 1);
        CHECK(run.of("overlay_open")[0]["source"] == "hotkey");
    }
    SUBCASE("the game is not running: ck3_not_running, never restart_ck3") {
        const Run run = rig.run("ck3 stopped\narm\nask keep the run alive\n");
        const std::vector<json> notices = run.of("notice");
        REQUIRE_FALSE(notices.empty());
        for (const json& notice : notices) {
            CHECK(notice["kind"] != "restart_ck3");
        }
        CHECK(notices.back()["kind"] == "ck3_not_running");
        CHECK(run.of("ck3").size() == 1);
        CHECK(run.of("ck3")[0]["running"] == false);
    }
    SUBCASE("refresh is an opening by the player too") {
        const Run run = rig.run("refresh\nask keep the run alive\n");
        REQUIRE(run.of("overlay_open").size() == 1);
        CHECK(run.of("overlay_open")[0]["source"] == "refresh");
        CHECK(run.of("notice").back()["kind"] == "restart_ck3");
    }
    SUBCASE("the passive detector: a long silence of the log while the game runs") {
        rig.config.options.passiveSilenceMs = 150;
        const Run run = rig.run("ask keep the run alive\n");
        const std::vector<json> notices = run.of("notice");
        REQUIRE_FALSE(notices.empty());
        CHECK(notices.back()["kind"] == "silent_soft");
    }
}

#endif

TEST_CASE("headless chat: a transcript file per question, only when asked for") {
    Rig rig;
    rig.config.logContent = true;
    rig.config.transcriptDir = rig.dir.path("transcripts");
    const Run run = rig.run("ask is my heir safe?\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitOk);
    REQUIRE(run.of("transcript").size() == 1);
    const fs::path path = run.of("transcript")[0]["path"].get<std::string>();
    REQUIRE(fs::exists(path));
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("--- question ---\nis my heir safe?\n") != std::string::npos);
    CHECK(text.str().find("--- answer ---\nCheck your heir.\n") != std::string::npos);
    CHECK(text.str().find("usage: input=500 output=12 cache_read=400 cache_write=0") !=
          std::string::npos);
    CHECK(text.str().find("snapshot: 3\n") != std::string::npos);
    CHECK(run.of("ready")[0]["transcripts"] == rig.dir.path("transcripts"));
    // The default stays off.
    Rig plain;
    const Run quiet = plain.run("ask hello\n");
    CHECK(quiet.of("transcript").empty());
}

TEST_CASE("headless chat: a transcript names the snapshot the question was asked with") {
    Rig rig(1);
    rig.config.logContent = true;
    rig.config.transcriptDir = rig.dir.path("transcripts");
    FakeProvider::Script slow;
    for (int i = 0; i < 15; ++i) {
        slow.tokens.push_back("w ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(60);
    rig.provider->setScript(slow);
    // A newer snapshot arrives while the answer streams: the transcript still names snapshot 1.
    std::thread writer([&rig] {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (rig.provider->callCount() == 0 && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::ofstream append(rig.log, std::ios::binary | std::ios::app);
        append << curia::bridge::test::stream(curia::bridge::test::exampleFrame(50));
    });
    const Run run = rig.run("ask hello\n");
    writer.join();
    REQUIRE(run.of("transcript").size() == 1);
    CHECK(run.of("snapshot").size() == 2);  // at ready, and the newer one while the answer ran
    const std::ifstream in(fs::path(run.of("transcript")[0]["path"].get<std::string>()),
                           std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    CHECK(text.str().find("snapshot: 1\n") != std::string::npos);
    CHECK(text.str().find("snapshot: 50\n") == std::string::npos);
}

#if !defined(_WIN32)
TEST_CASE("headless chat: a transcript folder that cannot be written to is refused up front") {
    Rig rig;
    rig.config.logContent = true;
    const fs::path folder = fs::path(rig.dir.path("locked"));
    fs::create_directories(folder);
    fs::permissions(folder, fs::perms::owner_read | fs::perms::owner_exec,
                    fs::perm_options::replace);
    rig.config.transcriptDir = folder.string();
    const Run run = rig.run("ask hello\n");
    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace);
    if (::geteuid() != 0) {  // root can write anywhere
        CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
        REQUIRE(run.of("fatal").size() == 1);
        CHECK(run.of("fatal")[0]["code"] == "transcript_refused");
        CHECK(rig.provider->callCount() == 0);
    }
}
#endif

TEST_CASE(
    "headless chat: a transcript folder inside a source tree is refused before anything is sent") {
    Rig rig;
    rig.config.logContent = true;
    const fs::path repo = fs::path(rig.dir.path("repo"));
    fs::create_directories(repo / ".git");
    rig.config.transcriptDir = (repo / "transcripts").string();
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
    REQUIRE(run.of("fatal").size() == 1);
    CHECK(run.of("fatal")[0]["code"] == "transcript_refused");
    CHECK(rig.provider->callCount() == 0);
    CHECK_FALSE(fs::exists(repo / "transcripts"));
}

TEST_CASE("headless chat: the time limit ends a long answer with exit 3") {
    Rig rig;
    rig.config.options.timeoutSeconds = 1;
    FakeProvider::Script slow;
    for (int i = 0; i < 100; ++i) {
        slow.tokens.push_back("w ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(100);
    rig.provider->setScript(slow);
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitTimeout);
    REQUIRE(run.of("fatal").size() == 1);
    CHECK(run.of("fatal")[0]["code"] == "timeout");
    CHECK(run.terminals() == 1);
}

TEST_CASE("headless chat: a frame written later opens the overlay") {
    Rig rig(1);
    rig.config.options.fromStart = false;
    FakeProvider::Script slow;
    for (int i = 0; i < 15; ++i) {
        slow.tokens.push_back("w ");
    }
    slow.onThread = true;
    slow.gap = std::chrono::milliseconds(60);
    rig.provider->setScript(slow);
    // The log is tailed from its end; a frame appended while the first question runs arrives
    // as a new snapshot with the overlay request of a game click.
    // The first question is only started once the run is ready, i.e. once the tailer has read the
    // log up to its end and so knows where "the end" is: that is the signal to append.
    std::thread writer([&rig] {
        const auto giveUp = std::chrono::steady_clock::now() + std::chrono::seconds(20);
        while (rig.provider->callCount() == 0 && std::chrono::steady_clock::now() < giveUp) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::ofstream append(rig.log, std::ios::binary | std::ios::app);
        append << curia::bridge::test::stream(curia::bridge::test::exampleFrame(7));
    });
    const Run run = rig.run("ask keep the run alive\n");
    writer.join();
    CHECK(run.of("ready")[0]["snapshot"].is_null());
    REQUIRE(run.of("snapshot").size() == 1);
    CHECK(run.of("snapshot")[0]["snap"] == 7);
    REQUIRE(run.of("overlay_open").size() == 1);
    CHECK(run.of("overlay_open")[0]["source"] == "game_click");
}

#if defined(CURIA_ENABLE_DEV_ENV)
TEST_CASE("headless chat: the self-test exception ends with a fatal event and no key") {
    Rig rig;
    rig.config.options.selftestThrow = true;
    const Run run = rig.run("ask hello\n");
    CHECK(run.result.exitCode == curia::kHeadlessExitInternal);
    REQUIRE(run.of("fatal").size() == 1);
    const json fatal = run.of("fatal")[0];
    CHECK(fatal["code"] == "internal_error");
    CHECK(fatal["message"].get<std::string>().find("internal error:") == 0);
    CHECK(run.out.find("FAKE") == std::string::npos);
    CHECK(run.err.find("FAKE") == std::string::npos);
    CHECK(run.err.find("selftest") != std::string::npos);
}
#else
TEST_CASE("headless chat: test-only options are refused in a build without them") {
    Rig rig;
    rig.config.options.tickMs = 10;
    const Run run = rig.run("");
    CHECK(run.result.exitCode == curia::kHeadlessExitSetup);
    CHECK(rig.provider->callCount() == 0);
}
#endif

TEST_CASE("fatal exception text: scrubbed, bounded, one line") {
    curia::SecretRegistry registry;
    REQUIRE(registry.add(kKey));
    const std::string text = curia::describeFatalException(
        ("broke on " + kKey + "\nsecond line\x01 " + std::string(500, 'x')).c_str(), &registry);
    CHECK(text.find("internal error: broke on [redacted]") == 0);
    CHECK(text.find('\n') == std::string::npos);
    CHECK(text.size() <= 16 + 300);
    CHECK(curia::describeFatalException(nullptr) == "internal error: ");
}
