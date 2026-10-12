// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "advisor_test_support.h"
#include "bridge/log_watcher.h"
#include "bridge_test_support.h"
#include "core/app_controller.h"
#include "core/app_log.h"
#include "core/dev_config.h"

using namespace curia;
using namespace curia::advisor;
using namespace curia::advisor::test;
using providers::ChatError;
using providers::TokenKind;
using providers::Usage;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::size_t count(const std::string& text, const std::string& part) {
    std::size_t n = 0;
    for (std::size_t at = text.find(part); at != std::string::npos;
         at = text.find(part, at + part.size())) {
        ++n;
    }
    return n;
}

class Recorder final : public AppObserver {
public:
    void onChatState(ChatState state) override { states.push_back(state); }
    void onTranscriptChanged(std::size_t /*index*/) override { ++transcriptChanges; }
    void onSnapshotChanged(const SnapshotInfo& info) override { snapshots.push_back(info); }
    void onOverlayRequested(bool probe) override { overlays.push_back(probe); }
    void onNoticeChanged(Notice notice) override { notices.push_back(notice); }
    void onUnsupportedFormat(std::string_view leading) override {
        unsupported.emplace_back(leading);
    }

    [[nodiscard]] std::size_t terminals() const {
        std::size_t n = 0;
        for (const ChatState state : states) {
            if (state == ChatState::Done || state == ChatState::Error ||
                state == ChatState::Cancelled) {
                ++n;
            }
        }
        return n;
    }

    std::vector<ChatState> states;
    std::size_t transcriptChanges = 0;
    std::vector<SnapshotInfo> snapshots;
    std::vector<bool> overlays;
    std::vector<Notice> notices;
    std::vector<std::string> unsupported;
};

struct Rig {
    static AppController::Options options(Rig& rig) {
        AppController::Options options;
        options.clock = rig.clock.fn();
        options.log = &rig.log;
        options.registry = &rig.registry;
        options.wake = [&rig] { ++rig.wakes; };
        return options;
    }

    static AppLog::Options logOptions(Rig& rig) {
        AppLog::Options options;
        options.registry = &rig.registry;
        return options;
    }

    Rig()
        : log(std::filesystem::path(dir.path("logs")), logOptions(*this)),
          controller(options(*this)) {
        controller.setObserver(&recorder);
        provider->setScript(defaultScript());
        REQUIRE(controller.setProvider(provider, "mock-model-1"));
    }

    static FakeProvider::Script defaultScript() {
        FakeProvider::Script script;
        script.tokens = {"Check ", "your heir."};
        script.usage.input = 500;
        script.usage.output = 12;
        script.usage.cacheRead = 400;
        script.usage.stopReason = "end_turn";
        return script;
    }

    void frame(const bridge::Frame& value) {
        controller.onMessage(UiMessage{makeWatcherEvent(bridge::FrameComplete{value})});
    }
    void damaged() {
        controller.onMessage(UiMessage{
            makeWatcherEvent(bridge::FrameDamaged{5, bridge::DamageReason::Incomplete, ""})});
    }
    void progress(std::uint64_t totalBytes) {
        bridge::TailerStats stats;
        stats.totalBytes = totalBytes;
        controller.onMessage(UiMessage{LogProgress{stats}});
    }

    std::string logText() const {
        bool ok = false;
        const std::string text = bridge::test::readFile(log.path().string(), ok);
        REQUIRE(ok);
        return text;
    }

    // The last user message of the request the n-th call received.
    std::string lastUserText(std::size_t call) {
        return provider->call(call).request.conversation.back().text;
    }

    FakeClock clock;
    std::atomic<int> wakes{0};
    SecretRegistry registry;
    bridge::test::TempDir dir;
    AppLog log;
    AppController controller;
    Recorder recorder;
    std::shared_ptr<FakeProvider> provider = std::make_shared<FakeProvider>();
};

}  // namespace

TEST_CASE("controller: frame in, overlay opens, ask, streamed answer, the data is in the request") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 4321, "Test Heir Two"));
    CHECK(rig.recorder.overlays.size() == 1);
    CHECK_FALSE(rig.recorder.overlays[0]);
    REQUIRE(rig.recorder.snapshots.size() == 1);
    CHECK(rig.recorder.snapshots[0].sequence == 1);
    CHECK(rig.recorder.snapshots[0].first);
    CHECK_FALSE(rig.recorder.snapshots[0].unreadable);
    CHECK(rig.controller.snapshot().hasSnapshot());

    REQUIRE((rig.controller.onUserAsk("Is my heir safe?") == AskOutcome::Started));
    REQUIRE(rig.provider->callCount() == 1);
    const providers::ChatRequest request = rig.provider->call(0).request;
    CHECK((request.model == "mock-model-1"));
    CHECK(contains(request.snapshotText, "Treasury: 4321 gold"));
    CHECK(contains(request.snapshotText, "Heir: \"Test Heir Two\""));
    CHECK(contains(request.snapshotText, "15 September 1066"));
    CHECK((request.conversation.back().text == "Is my heir safe?"));

    CHECK(rig.controller.pump() >= 1);
    CHECK((rig.controller.session().state() == ChatState::Done));
    CHECK((rig.controller.session().transcript()[1].text == "Check your heir."));
    CHECK(rig.recorder.terminals() == 1);
    CHECK((rig.recorder.states ==
           std::vector<ChatState>{ChatState::Waiting, ChatState::Streaming, ChatState::Done}));
}

TEST_CASE("controller: not configured names the variables and starts nothing") {
    FakeClock clock;
    AppController::Options options;
    options.clock = clock.fn();
    AppController controller(options);
    CHECK_FALSE(controller.configured());

    SecretRegistry registry;
    const std::string key = "sk-ant-FAKE-test-key-0000";
    const platform::GetEnv env = [&](const char* name) -> std::string {
        return std::string(name) == "CURIA_ANTHROPIC_API_KEY" ? key : std::string();
    };
    const DevConfig config = loadDevConfig(env, registry);
    const std::string message = describeNotConfigured(config);
    controller.setNotConfigured(message);
    CHECK_FALSE(controller.configured());
    CHECK((controller.notConfiguredMessage() == message));
#if defined(CURIA_ENABLE_DEV_ENV)
    CHECK(contains(controller.notConfiguredMessage(), "CURIA_MODEL"));
#endif
    CHECK_FALSE(contains(controller.notConfiguredMessage(), key));

    CHECK((controller.onUserAsk("Hello?") == AskOutcome::NoProvider));
    CHECK(controller.session().transcript().empty());

    // The default text when nothing is given.
    AppController other(options);
    other.setNotConfigured("");
    CHECK_FALSE(other.notConfiguredMessage().empty());
}

TEST_CASE("controller: setProvider refuses an empty model or a missing provider") {
    Rig rig;
    CHECK_FALSE(rig.controller.setProvider(nullptr, "mock-model-1"));
    CHECK_FALSE(rig.controller.setProvider(rig.provider, ""));
    CHECK(rig.controller.configured());  // the earlier provider stays
    rig.controller.setNotConfigured("not set up");
    CHECK_FALSE(rig.controller.configured());
    CHECK((rig.controller.onUserAsk("Q") == AskOutcome::NoProvider));
    CHECK(rig.provider->callCount() == 0);
}

TEST_CASE("controller: clearing the provider stops what is in flight") {
    Rig rig;
    rig.provider->clearScript();
    REQUIRE((rig.controller.onUserAsk("Q") == AskOutcome::Started));
    rig.controller.setNotConfigured("gone");
    CHECK((rig.controller.session().state() == ChatState::Cancelled));
    CHECK(rig.provider->call(0).cancel.cancelled());
    CHECK(rig.recorder.terminals() == 1);
}

TEST_CASE("controller: replacing the provider stops what is in flight, and only once") {
    Rig rig;
    rig.provider->clearScript();
    REQUIRE((rig.controller.onUserAsk("Q") == AskOutcome::Started));
    const auto oldCall = rig.provider->call(0);
    auto replacement = std::make_shared<FakeProvider>();
    replacement->setScript(Rig::defaultScript());
    REQUIRE(rig.controller.setProvider(replacement, "other-model"));
    CHECK((rig.controller.session().state() == ChatState::Cancelled));
    CHECK(oldCall.cancel.cancelled());
    CHECK(rig.recorder.terminals() == 1);
    // The old provider's late callbacks change nothing.
    oldCall.handlers.onToken(TokenKind::Text, "late");
    oldCall.handlers.onError(ChatError{});
    rig.controller.pump();
    CHECK(rig.recorder.terminals() == 1);
    // The next ask goes to the new provider with the new model.
    REQUIRE((rig.controller.onUserAsk("Q2") == AskOutcome::Started));
    REQUIRE(replacement->callCount() == 1);
    CHECK((replacement->call(0).request.model == "other-model"));
    CHECK(rig.provider->callCount() == 1);
}

TEST_CASE("controller: the generation in the app log is the same for one ask") {
    Rig rig;
    rig.provider->clearScript();
    rig.controller.onUserAsk("Q1");
    rig.provider->call(0).handlers.onToken(TokenKind::Text, "part ");
    rig.controller.pump();
    rig.controller.onStop();
    const std::string log = rig.logText();
    // ask_started, ask_first_token and ask_cancelled all say gen=1.
    CHECK(count(log, "gen=1") == 3);
    CHECK(count(log, "gen=2") == 0);
    rig.controller.onUserAsk("Q2");
    rig.provider->call(1).handlers.onToken(TokenKind::Text, "again ");
    rig.provider->call(1).handlers.onDone(Usage{});
    rig.controller.pump();
    CHECK(count(rig.logText(), "gen=3") == 3);  // started, first token, done
}

TEST_CASE("controller: stop keeps the partial answer, late callbacks are ignored") {
    Rig rig;
    rig.provider->clearScript();
    rig.controller.onUserAsk("Q");
    const auto handlers = rig.provider->call(0).handlers;
    handlers.onToken(TokenKind::Text, "Start");
    rig.controller.pump();
    rig.controller.onStop();
    handlers.onToken(TokenKind::Text, " LATE");
    handlers.onDone(Usage{});
    rig.controller.pump();
    CHECK((rig.controller.session().transcript()[1].text == "Start"));
    CHECK((rig.controller.session().state() == ChatState::Cancelled));
    CHECK(rig.recorder.terminals() == 1);
    CHECK(count(rig.logText(), "ask_cancelled") == 1);
    CHECK(count(rig.logText(), "ask_done") == 0);
}

TEST_CASE("controller: retry after an error asks the same question again") {
    Rig rig;
    FakeProvider::Script failing;
    failing.tokens = {"half"};
    ChatError error;
    error.code = ChatError::Code::Network;
    error.message = "connection reset";
    error.retryable = true;
    failing.error = error;
    rig.provider->setScript(failing);
    rig.controller.onUserAsk("Question one");
    rig.controller.pump();
    CHECK((rig.controller.session().state() == ChatState::Error));
    CHECK(contains(bridge::test::must(rig.controller.session().transcript()[1].error).summary,
                   "Could not reach"));

    rig.provider->setScript(Rig::defaultScript());
    CHECK((rig.controller.onRetry() == AskOutcome::Started));
    rig.controller.pump();
    CHECK((rig.controller.session().state() == ChatState::Done));
    CHECK(rig.controller.session().transcript().size() == 2);
    CHECK((rig.controller.session().transcript()[1].text == "Check your heir."));
    CHECK(rig.provider->callCount() == 2);
    CHECK((rig.provider->call(1).request.conversation.back().text == "Question one"));
    CHECK(rig.recorder.terminals() == 2);  // one per ask
}

TEST_CASE("controller: a new frame adds a divider and keeps the chat") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    CHECK(rig.controller.session().transcript().empty());  // nothing to divide yet
    rig.controller.onUserAsk("Q1");
    rig.controller.pump();
    rig.frame(frameWithGoldAndHeir(1066, 10, 1, 250, "Test Heir Two"));
    const auto& transcript = rig.controller.session().transcript();
    REQUIRE(transcript.size() == 3);
    CHECK((transcript[2].kind == EntryKind::Divider));
    CHECK(contains(transcript[2].text, "1 October 1066"));
    CHECK_FALSE(contains(transcript[2].text, "save"));

    // The next question carries the new snapshot and the old exchange.
    rig.controller.onUserAsk("Q2");
    const providers::ChatRequest request = rig.provider->call(1).request;
    CHECK(contains(request.snapshotText, "Treasury: 250 gold"));
    REQUIRE(request.conversation.size() == 3);
    CHECK((request.conversation[0].text == "Q1"));

    // A date that goes back is mentioned.
    rig.controller.pump();
    rig.frame(frameWithGoldAndHeir(1066, 9, 1, 250, "Test Heir Two"));
    CHECK(
        contains(rig.controller.session().transcript().back().text, "a save may have been loaded"));
    CHECK(rig.recorder.snapshots.back().dateWentBack);
    CHECK(rig.recorder.overlays.size() == 3);
}

TEST_CASE("controller: a frame in the middle of an answer does not disturb it") {
    Rig rig;
    rig.provider->clearScript();
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    rig.controller.onUserAsk("Q1");
    const auto handlers = rig.provider->call(0).handlers;
    handlers.onToken(TokenKind::Text, "Part one. ");
    rig.controller.pump();
    rig.frame(frameWithGoldAndHeir(1066, 9, 16, 999, "Test Heir Two"));
    handlers.onToken(TokenKind::Text, "Part two.");
    handlers.onDone(Usage{});
    rig.controller.pump();
    CHECK((rig.controller.session().transcript()[1].text == "Part one. Part two."));
    // The request that was sent keeps the snapshot it was built with.
    CHECK(contains(rig.provider->call(0).request.snapshotText, "Treasury: 100 gold"));
}

TEST_CASE("controller: freshness reaches the request as a note on the latest message only") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    rig.progress(1000);

    // Fresh: no note.
    rig.clock.advance(std::chrono::seconds(30));
    rig.controller.onUserAsk("Q1");
    rig.controller.pump();
    CHECK((rig.lastUserText(0) == "Q1"));

    // Seven minutes later, the log silent for as long: both are mentioned.
    rig.clock.advance(std::chrono::seconds(7 * 60));
    rig.controller.onUserAsk("Q2");
    const std::string note = rig.lastUserText(1);
    CHECK(note.rfind("Q2\n\n[Context: ", 0) == 0);
    CHECK(contains(note, "game date 15 September 1066"));
    CHECK(contains(note, "taken 7 minutes ago"));
    CHECK(contains(note, "no new game data for 7 minutes"));
    // The first question went to the history without a note.
    CHECK((rig.provider->call(1).request.conversation[0].text == "Q1"));
    rig.controller.pump();

    // A damaged frame: the export is unreadable.
    rig.damaged();
    CHECK(rig.recorder.snapshots.back().unreadable);
    rig.progress(2000);
    rig.controller.onUserAsk("Q3");
    CHECK(contains(rig.lastUserText(2), "could not be read"));
    rig.controller.pump();

    // The next good frame clears it, and the note with it.
    rig.frame(frameWithGoldAndHeir(1066, 9, 17, 100, "Test Heir Two"));
    rig.controller.onUserAsk("Q4");
    CHECK((rig.lastUserText(3) == "Q4"));
}

TEST_CASE("controller: with no snapshot the question goes out with the none-yet block") {
    Rig rig;
    rig.controller.onUserAsk("How do I start?");
    CHECK(contains(rig.provider->call(0).request.snapshotText, "No snapshot yet"));
}

TEST_CASE("controller: a damaged frame keeps the old snapshot and flags it") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    rig.damaged();
    CHECK(rig.controller.snapshot().unreadable());
    CHECK(rig.controller.snapshot().hasSnapshot());
    CHECK(contains(rig.controller.snapshot().text(), "Treasury: 100 gold"));
    CHECK(rig.recorder.overlays.size() == 1);  // a damaged frame does not open the overlay
    CHECK(contains(rig.logText(), "frame_damaged"));

    rig.controller.onMessage(
        UiMessage{makeWatcherEvent(bridge::UnsupportedFormat{9, "0|LEADING-FIELD|x"})});
    REQUIRE(rig.recorder.unsupported.size() == 1);
    CHECK((rig.recorder.unsupported[0] == "0|LEADING-FIELD|x"));
    CHECK(contains(rig.logText(), "frame_unsupported"));
    CHECK_FALSE(contains(rig.logText(), "LEADING-FIELD"));
}

TEST_CASE("controller: an unsupported format marks the export unreadable and arms nothing else") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    CHECK_FALSE(rig.controller.snapshot().unreadable());
    rig.controller.onMessage(
        UiMessage{makeWatcherEvent(bridge::UnsupportedFormat{9, "0|LEADING-FIELD|x"})});
    CHECK(rig.controller.snapshot().unreadable());
    REQUIRE(rig.recorder.snapshots.size() == 2);
    CHECK(rig.recorder.snapshots[1].unreadable);
    // The next ask tells the model about it.
    rig.controller.onUserAsk("Q");
    CHECK(contains(rig.lastUserText(0), "could not be read"));
}

TEST_CASE("controller: a damaged frame also disarms the silence detector") {
    Rig rig;
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir Two"));
    rig.controller.onCk3Running(true);
    rig.clock.advance(std::chrono::seconds(200));  // the snapshot is old
    rig.controller.onOverlayOpened(OverlaySource::Hotkey);
    rig.clock.advance(std::chrono::seconds(5));
    rig.damaged();  // something did arrive: the export is alive, though unreadable
    rig.clock.advance(std::chrono::seconds(100));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() != Notice::RestartCk3));
}

TEST_CASE("controller: Options::maxTokens reaches the request") {
    AppController::Options options;
    options.maxTokens = 777;
    options.wake = [] {};
    AppController controller(options);
    auto provider = std::make_shared<FakeProvider>();
    REQUIRE(controller.setProvider(provider, "mock-model-1"));
    REQUIRE((controller.onUserAsk("Q") == AskOutcome::Started));
    REQUIRE(provider->callCount() == 1);
    CHECK(provider->call(0).request.maxTokens == 777);
}

TEST_CASE("controller: a probe frame opens the overlay, labelled") {
    Rig rig;
    rig.frame(emptyFrame());  // mode 1
    REQUIRE(rig.recorder.overlays.size() == 1);
    CHECK(rig.recorder.overlays[0]);
    CHECK(rig.recorder.snapshots[0].probe);
}

TEST_CASE("controller: the silence notices, armed by the player and not by the game click") {
    Rig rig;
    rig.controller.onOverlayOpened(OverlaySource::GameClick);
    rig.controller.onTick(rig.clock.now + std::chrono::seconds(60));
    CHECK((rig.controller.notice() == Notice::None));

    rig.controller.onOverlayOpened(OverlaySource::Hotkey);
    rig.clock.advance(std::chrono::seconds(14));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::None));
    rig.clock.advance(std::chrono::seconds(1));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::RestartCk3));
    REQUIRE(rig.recorder.notices.size() == 1);
    CHECK((rig.recorder.notices[0] == Notice::RestartCk3));

    // CK3 not running wins, and a frame later clears everything.
    rig.controller.onCk3Running(false);
    CHECK((rig.controller.notice() == Notice::Ck3NotRunning));
    rig.controller.onCk3Running(true);
    CHECK((rig.controller.notice() == Notice::None));
    CHECK((rig.recorder.notices ==
           std::vector<Notice>{Notice::RestartCk3, Notice::Ck3NotRunning, Notice::None}));
    CHECK(contains(rig.logText(), "name=RestartCk3"));
}

TEST_CASE("controller: a frame disarms the detector, log growth alone does not") {
    Rig rig;
    rig.controller.onOverlayOpened(OverlaySource::Tray);
    rig.clock.advance(std::chrono::seconds(10));
    rig.progress(500);
    rig.frame(minimalFrame());
    rig.clock.advance(std::chrono::seconds(60));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::None));

    Rig other;
    other.controller.onOverlayOpened(OverlaySource::Refresh);
    other.clock.advance(std::chrono::seconds(10));
    other.progress(500);
    other.clock.advance(std::chrono::seconds(10));
    other.controller.onTick(other.clock.now);
    CHECK((other.controller.notice() == Notice::RestartCk3));
}

TEST_CASE("controller: the passive notice follows the log, a truncation counts as growth") {
    Rig rig;
    rig.progress(100);
    rig.clock.advance(std::chrono::seconds(599));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::None));
    rig.clock.advance(std::chrono::seconds(1));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::SilentSoft));
    // The same total again is not growth ...
    rig.progress(100);
    CHECK((rig.controller.notice() == Notice::SilentSoft));
    // ... more bytes are.
    rig.progress(150);
    CHECK((rig.controller.notice() == Notice::None));

    rig.clock.advance(std::chrono::seconds(700));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::SilentSoft));
    rig.controller.onMessage(UiMessage{LogNotice{bridge::TailerNotice::Truncated}});
    CHECK((rig.controller.notice() == Notice::None));
    CHECK(contains(rig.logText(), "log_truncated"));
    // After a truncation the totals start from 0 again: 20 bytes is growth.
    rig.clock.advance(std::chrono::seconds(700));
    rig.controller.onTick(rig.clock.now);
    CHECK((rig.controller.notice() == Notice::SilentSoft));
    rig.progress(20);
    CHECK((rig.controller.notice() == Notice::None));
}

TEST_CASE("controller: the app log holds numbers and words, never text") {
    Rig rig;
    rig.provider->clearScript();
    rig.frame(frameWithGoldAndHeir(1066, 9, 15, 100, "Zorblax The Distinctive"));
    rig.controller.onUserAsk("PURPLE-ELEPHANT-QUESTION about my realm");
    const auto handlers = rig.provider->call(0).handlers;
    handlers.onToken(TokenKind::Text, "MAGENTA-GIRAFFE-ANSWER");
    Usage usage;
    usage.input = 700;
    usage.output = 33;
    usage.cacheRead = 600;
    usage.cacheWrite = 5;
    usage.stopReason = "max_tokens";
    handlers.onDone(usage);
    rig.controller.pump();

    rig.controller.onUserAsk("second");
    ChatError error;
    error.code = ChatError::Code::Auth;
    error.message = "RAW-SERVER-WORDS sk-ant-FAKE-test-key-0000";
    error.httpStatus = 401;
    rig.provider->call(1).handlers.onError(error);
    rig.controller.pump();

    const std::string log = rig.logText();
    CHECK_FALSE(contains(log, "PURPLE"));
    CHECK_FALSE(contains(log, "MAGENTA"));
    CHECK_FALSE(contains(log, "Zorblax"));
    CHECK_FALSE(contains(log, "Test Ruler"));
    CHECK_FALSE(contains(log, "RAW-SERVER"));
    CHECK_FALSE(contains(log, "FAKE-test"));
    CHECK_FALSE(contains(log, "heir"));
    CHECK(contains(log, "ask_started"));
    CHECK(contains(log, "ask_done"));
    CHECK(contains(log, "input=700"));
    CHECK(contains(log, "output=33"));
    CHECK(contains(log, "cache_read=600"));
    CHECK(contains(log, "max_tokens=true"));
    CHECK(contains(log, "ask_error"));
    CHECK(contains(log, "code=Auth"));
    CHECK(contains(log, "status=401"));
    CHECK(contains(log, "persona_version="));
    CHECK(contains(log, "frame_complete"));
    // One terminal line per ask.
    CHECK(count(log, "ask_done") + count(log, "ask_error") + count(log, "ask_cancelled") == 2);
    CHECK(count(log, "ask_started") == 2);
}

TEST_CASE("controller: an answer that repeats the key reaches neither the transcript nor the log") {
    Rig rig;
    const std::string key = "sk-ant-FAKE-test-key-0000";
    REQUIRE(rig.registry.add(key));
    rig.provider->clearScript();
    rig.controller.onUserAsk("Q");
    const auto handlers = rig.provider->call(0).handlers;
    handlers.onToken(TokenKind::Text, "Here: sk-ant-FAKE-te");
    rig.controller.pump();
    handlers.onToken(TokenKind::Text, "st-key-0000.");
    handlers.onDone(Usage{});
    rig.controller.pump();
    CHECK_FALSE(contains(rig.controller.session().transcript()[1].text, "FAKE-test"));
    CHECK_FALSE(contains(rig.logText(), "FAKE-test"));
}

TEST_CASE("controller: a key split across two chunks is hidden in the text and the log") {
    Rig rig;
    const std::string key = "sk-ant-api03-FAKEFAKEFAKEFAKEFAKE-test-0123456789ABCDEFGHIJ";
    REQUIRE(rig.registry.add(key));
    rig.provider->clearScript();
    rig.controller.onUserAsk("Q");
    const auto handlers = rig.provider->call(0).handlers;
    handlers.onToken(TokenKind::Text, "Your key: " + key.substr(0, 33));
    rig.controller.pump();
    handlers.onToken(TokenKind::Text, key.substr(33) + " is set.");
    handlers.onDone(Usage{});
    rig.controller.pump();
    const std::string& text = rig.controller.session().transcript()[1].text;
    for (const char* part : {"FAKEFAKE", "api03", "0123456789", "ABCDEFGHIJ", "test-"}) {
        CAPTURE(part);
        CHECK_FALSE(contains(text, part));
        CHECK_FALSE(contains(rig.logText(), part));
    }
    CHECK((text == "Your key: [redacted] is set."));
}

TEST_CASE("controller: the refusals are logged by outcome only") {
    Rig rig;
    rig.provider->clearScript();
    CHECK((rig.controller.onUserAsk("   ") == AskOutcome::EmptyText));
    rig.controller.onUserAsk("Q");
    CHECK((rig.controller.onUserAsk("again") == AskOutcome::Busy));
    CHECK(contains(rig.logText(), "outcome=EmptyText"));
    CHECK(contains(rig.logText(), "outcome=Busy"));
}

TEST_CASE("controller: watcher callbacks post into the mailbox and wake the UI once") {
    Rig rig;
    const bridge::LogWatcher::Callbacks callbacks = rig.controller.watcherCallbacks();
    bridge::Event event = bridge::FrameComplete{minimalFrame()};
    callbacks.onNotice(bridge::TailerNotice::Attached);
    callbacks.onEvent(std::move(event));
    callbacks.onProgress(bridge::TailerStats{});
    CHECK(rig.wakes.load() == 1);
    CHECK(rig.controller.mailbox()->size() == 3);
    CHECK(rig.controller.pump() == 3);
    CHECK(rig.controller.snapshot().hasSnapshot());
    CHECK(rig.controller.mailbox()->empty());
}

TEST_CASE("controller end to end: log watcher thread, mailbox, streamed answer") {
    Rig rig;
    auto file = std::make_shared<bridge::test::FakeFile>();
    file->append(bridge::test::stream(bridge::test::exampleFrame(1)));
    bridge::TailerOptions tailerOptions;
    tailerOptions.fromStart = true;
    bridge::LogWatcher watcher(
        std::make_unique<bridge::LogTailer>(std::make_unique<bridge::test::FakeFileSource>(file),
                                            tailerOptions),
        rig.controller.watcherCallbacks(), std::chrono::milliseconds(5));
    REQUIRE(watcher.start());

    // The UI loop: wait for the wake-up, pump.
    REQUIRE(waitUntil([&] {
        rig.controller.pump();
        return !rig.recorder.overlays.empty();
    }));
    watcher.stop();
    CHECK(rig.controller.snapshot().hasSnapshot());

    rig.provider->setScript([] {
        FakeProvider::Script script = Rig::defaultScript();
        script.onThread = true;
        script.gap = std::chrono::milliseconds(1);
        return script;
    }());
    REQUIRE((rig.controller.onUserAsk("What about my gold and heir?") == AskOutcome::Started));
    REQUIRE(waitUntil([&] {
        rig.controller.pump();
        return rig.controller.session().state() == ChatState::Done;
    }));

    // The fixture's gold and heir were in what the provider received.
    const providers::ChatRequest request = rig.provider->call(0).request;
    CHECK(contains(request.snapshotText, "Treasury: 312 gold"));
    CHECK(contains(request.snapshotText, "Heir: \"Test Heir Two\""));
    CHECK(contains(request.snapshotText, "\"Test Vassal Three\""));
    CHECK((rig.controller.session().transcript()[1].text == "Check your heir."));
    CHECK(rig.recorder.terminals() == 1);
    CHECK(rig.wakes.load() >= 2);
}

TEST_CASE("controller: messages dropped by the mailbox limit are counted in the log") {
    Rig rig;
    const auto box = rig.controller.mailbox();
    for (std::size_t i = 0; i < UiMailbox::kDefaultMaxMessages + 10; ++i) {
        box->push(UiMessage{LogNotice{bridge::TailerNotice::Attached}});
    }
    CHECK(box->dropped() == 10);
    CHECK(rig.controller.pump() == UiMailbox::kDefaultMaxMessages);
    CHECK(contains(rig.logText(), "mailbox_dropped"));
    CHECK(contains(rig.logText(), "total=10"));
    // Reported once, not at every pump.
    box->push(UiMessage{LogNotice{bridge::TailerNotice::Attached}});
    rig.controller.pump();
    CHECK(count(rig.logText(), "mailbox_dropped") == 1);
}
