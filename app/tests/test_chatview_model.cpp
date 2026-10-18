// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "advisor_test_support.h"
#include "chatview/canned_conversation.h"
#include "chatview/log_path.h"
#include "chatview/view_model.h"
#include "chatview/view_text.h"
#include "core/app_controller.h"

using namespace curia;
using namespace curia::chatview;
using advisor::ChatState;
using advisor::test::FakeClock;
using canned::ScriptedProvider;

namespace {

// A controller with a scripted provider that moves one piece per step().
struct Rig {
    explicit Rig(std::vector<ScriptedProvider::Answer> script)
        : provider(std::make_shared<ScriptedProvider>(std::move(script))),
          controller(options(*this)) {
        REQUIRE(controller.setProvider(provider, "mock-model-1"));
    }

    static AppController::Options options(Rig& rig) {
        AppController::Options options;
        options.clock = rig.clock.fn();
        options.registry = &rig.registry;
        return options;
    }

    void frame(const bridge::Frame& value) {
        controller.onMessage(UiMessage{makeWatcherEvent(bridge::FrameComplete{value})});
    }

    // One piece (or the end) of the answer in flight, handled by the controller.
    void step() {
        provider->step();
        controller.pump();
    }

    void finish() {
        while (provider->busy()) {
            step();
        }
    }

    ChatView view() const { return buildChatView(viewInputs(controller, facts)); }

    FakeClock clock;
    SecretRegistry registry;
    std::shared_ptr<ScriptedProvider> provider;
    AppController controller;
    WindowFacts facts;
};

ScriptedProvider::Answer answer(std::vector<std::string> pieces) {
    ScriptedProvider::Answer value;
    value.pieces = std::move(pieces);
    value.usage.stopReason = "end_turn";
    return value;
}

ScriptedProvider::Answer failing(std::vector<std::string> pieces, providers::ChatError::Code code,
                                 bool retryable) {
    ScriptedProvider::Answer value;
    value.pieces = std::move(pieces);
    providers::ChatError error;
    error.code = code;
    error.retryable = retryable;
    value.error = error;
    return value;
}

std::string shown(const ViewItem& item) {
    return plainText(item.answer);
}

bool hasBanner(const ChatView& view, std::string_view text) {
    return std::any_of(view.banners.begin(), view.banners.end(),
                       [&](const Banner& banner) { return banner.text == text; });
}

}  // namespace

TEST_CASE("view: nothing read yet, the pause reminder and the input hint") {
    Rig rig({answer({"Hi."})});
    const ChatView view = rig.view();
    // The empty conversation says what to do; no status line repeats it.
    CHECK((view.emptyState == text::kNothingReadYet));
    CHECK(view.status.empty());
    CHECK((view.statusTone == Tone::Plain));
    CHECK(view.banners.empty());
    CHECK(view.items.empty());
    REQUIRE(view.pauseReminder.lines.size() == 1);
    CHECK((plainText(view.pauseReminder) ==
           std::string(text::kPauseReminderLead) + std::string(text::kPauseReminderRest)));
    CHECK(view.pauseReminder.lines[0].runs.at(0).bold);
    CHECK_FALSE(view.pauseReminder.lines[0].runs.at(1).bold);
    CHECK((view.inputHint == text::kInputHint));
    // Nothing typed: Send is off; something typed: on.
    CHECK_FALSE(view.canSend);
    rig.facts.draftEmpty = false;
    CHECK(rig.view().canSend);
    CHECK_FALSE(rig.view().canStop);
    CHECK_FALSE(rig.view().spinnerActive);
}

TEST_CASE("view: the status names the date of what can be seen") {
    Rig rig({answer({"Hi."})});
    rig.frame(canned::firstFrame());
    CHECK((rig.view().status == "What I can see is from 3 October 1071."));
    CHECK((rig.view().status == text::seenFrom({1071, 10, 3})));
    CHECK(rig.view().emptyState.empty());
}

TEST_CASE("view: nothing read yet, once the conversation has rows, is the status line") {
    Rig rig({answer({"Hi."})});
    REQUIRE((rig.controller.onUserAsk("Can you see my realm?") == advisor::AskOutcome::Started));
    rig.finish();
    const ChatView view = rig.view();
    CHECK((view.status == text::kNothingReadYet));
    CHECK(view.emptyState.empty());
}

TEST_CASE("view: not configured shows the message, Send is off, the realm status still works") {
    FakeClock clock;
    AppController::Options options;
    options.clock = clock.fn();
    AppController controller(options);
    controller.setNotConfigured("Curia is not configured. CURIA_MODEL is not set.");
    controller.onMessage(UiMessage{makeWatcherEvent(bridge::FrameComplete{canned::firstFrame()})});
    WindowFacts facts;
    facts.draftEmpty = false;
    const ChatView view = buildChatView(viewInputs(controller, facts));
    REQUIRE(view.banners.size() == 1);
    CHECK((view.banners[0].text == "Curia is not configured. CURIA_MODEL is not set."));
    CHECK((view.banners[0].tone == Tone::Warning));
    CHECK_FALSE(view.canSend);
    CHECK((view.status == "What I can see is from 3 October 1071."));
    // Without a message of its own, the default text.
    const ViewInputs inputs{};
    CHECK((buildChatView(inputs).banners.at(0).text == kDefaultNotConfigured));
}

TEST_CASE("view: thinking, then streaming, then a finished answer with Copy") {
    Rig rig({answer({"Your ", "**heir", "** is ", "safe."})});
    rig.frame(canned::firstFrame());
    rig.facts.draftEmpty = false;
    REQUIRE((rig.controller.onUserAsk("Is my heir safe?") == advisor::AskOutcome::Started));

    ChatView view = rig.view();
    REQUIRE(view.items.size() == 2);
    CHECK((view.items[0].kind == ItemKind::Question));
    CHECK((view.items[0].text == "Is my heir safe?"));
    CHECK((view.items[1].kind == ItemKind::Answer));
    CHECK((view.items[1].state == AnswerState::Thinking));
    CHECK(view.spinnerActive);
    CHECK(view.canStop);
    CHECK_FALSE(view.canSend);
    CHECK_FALSE(view.items[1].offerCopy);

    rig.step();  // "Your " (the session shows it once the blank arrives)
    view = rig.view();
    CHECK((view.items[1].state == AnswerState::Streaming));
    CHECK(view.spinnerActive);  // the dot after the text that is still arriving
    CHECK((shown(view.items[1]) == "Your"));
    rig.step();  // "**heir": bold while it is open, held back as the last word
    rig.step();  // "** is "
    view = rig.view();
    CHECK((shown(view.items[1]) == "Your heir is"));
    REQUIRE(view.items[1].answer.lines.size() == 1);
    CHECK(view.items[1].answer.lines[0].runs.at(1).bold);
    CHECK_FALSE(view.items[1].offerCopy);

    rig.finish();
    view = rig.view();
    CHECK((view.items[1].state == AnswerState::Complete));
    CHECK_FALSE(view.spinnerActive);
    CHECK((shown(view.items[1]) == "Your heir is safe."));
    CHECK(view.items[1].note.empty());
    CHECK(view.items[1].offerCopy);
    CHECK_FALSE(view.items[1].offerRetry);
    CHECK_FALSE(view.canStop);
    CHECK(view.canSend);
}

TEST_CASE("view: an error with Retry, only on the last answer and only when retryable") {
    Rig rig({failing({"Two ", "groups "}, providers::ChatError::Code::Overloaded, true),
             failing({}, providers::ChatError::Code::Auth, false),
             failing({}, providers::ChatError::Code::Network, true)});
    rig.frame(canned::firstFrame());
    REQUIRE((rig.controller.onUserAsk("Who might rebel?") == advisor::AskOutcome::Started));
    rig.finish();
    ChatView view = rig.view();
    REQUIRE(view.items.size() == 2);
    const ViewItem& failed = view.items[1];
    CHECK((failed.state == AnswerState::Failed));
    CHECK((shown(failed) == "Two groups"));  // the partial text stays
    CHECK(failed.noteIsError);
    CHECK((failed.note == "The service is busy right now. Try again in a moment."));
    CHECK(failed.offerRetry);
    CHECK(failed.offerCopy);

    // A new question: the earlier error no longer offers Retry; a key problem never does.
    REQUIRE((rig.controller.onUserAsk("And now?") == advisor::AskOutcome::Started));
    CHECK_FALSE(rig.view().items[1].offerRetry);  // in flight
    rig.finish();
    view = rig.view();
    REQUIRE(view.items.size() == 4);
    CHECK_FALSE(view.items[1].offerRetry);
    CHECK((view.items[3].state == AnswerState::Failed));
    CHECK_FALSE(view.items[3].offerRetry);
    CHECK_FALSE(view.items[3].offerCopy);  // no text
    CHECK((view.items[3].note.find("API key") != std::string::npos));

    // Retry replaces the last answer in place.
    REQUIRE((rig.controller.onRetry() == advisor::AskOutcome::Started));
    rig.finish();
    view = rig.view();
    REQUIRE(view.items.size() == 4);
    CHECK(view.items[3].offerRetry);  // a network error is retryable
}

TEST_CASE("view: a stopped answer keeps its text and says so") {
    Rig rig({answer({"First ", "part ", "and more."})});
    REQUIRE((rig.controller.onUserAsk("Long answer please") == advisor::AskOutcome::Started));
    rig.step();
    rig.step();
    rig.controller.onStop();
    rig.finish();  // the cancelled request ends; nothing changes on screen
    const ChatView view = rig.view();
    REQUIRE(view.items.size() == 2);
    CHECK((view.items[1].state == AnswerState::Stopped));
    CHECK((view.items[1].note == text::kStopped));
    CHECK((shown(view.items[1]) == "First part"));
    CHECK(view.items[1].offerCopy);
    CHECK_FALSE(view.items[1].offerRetry);
}

TEST_CASE("view: the length-limit note") {
    ScriptedProvider::Answer cut = answer({"A long answer"});
    cut.usage.stopReason = "max_tokens";
    Rig rig({cut});
    REQUIRE((rig.controller.onUserAsk("Tell me everything") == advisor::AskOutcome::Started));
    rig.finish();
    const ChatView view = rig.view();
    CHECK((view.items.at(1).state == AnswerState::Complete));
    CHECK((view.items.at(1).note == text::kCutShort));
    CHECK_FALSE(view.items.at(1).noteIsError);
}

TEST_CASE("view: a newer reading adds a divider row") {
    Rig rig({answer({"Fine."})});
    rig.frame(canned::firstFrame());
    REQUIRE((rig.controller.onUserAsk("How is it?") == advisor::AskOutcome::Started));
    rig.finish();
    rig.frame(canned::secondFrame());
    const ChatView view = rig.view();
    REQUIRE(view.items.size() == 3);
    CHECK((view.items[2].kind == ItemKind::Divider));
    CHECK((view.items[2].text == "Updated from the game: 14 November 1071"));
    CHECK((view.items[2].text == dividerLabel({1071, 11, 14}, false)));
    CHECK((view.status == "What I can see is from 14 November 1071."));
}

TEST_CASE("view: unreadable, version mismatch and the probe button") {
    Rig rig({answer({"x"})});
    rig.controller.onMessage(
        UiMessage{makeWatcherEvent(bridge::FrameDamaged{3, bridge::DamageReason::Incomplete, ""})});
    CHECK(hasBanner(rig.view(), text::kUnreadable));
    CHECK((rig.view().emptyState == text::kNothingReadYet));

    // A format this app does not read: the mismatch replaces the unreadable notice.
    rig.controller.onMessage(
        UiMessage{makeWatcherEvent(bridge::UnsupportedFormat{4, "BEGIN|9|9.9.9"})});
    rig.facts.onUnsupportedFormat();
    ChatView view = rig.view();
    CHECK(hasBanner(view, text::kVersionMismatch));
    CHECK_FALSE(hasBanner(view, text::kUnreadable));

    // A readable reading clears both; one from the test button says so.
    bridge::Frame probe = canned::firstFrame();
    probe.mode = 1;
    rig.frame(probe);
    SnapshotInfo info;
    info.unreadable = false;
    rig.facts.onSnapshotChanged(info);
    view = rig.view();
    CHECK_FALSE(hasBanner(view, text::kVersionMismatch));
    CHECK_FALSE(hasBanner(view, text::kUnreadable));
    CHECK(hasBanner(view, text::kProbe));
    rig.frame(canned::secondFrame());
    CHECK_FALSE(hasBanner(rig.view(), text::kProbe));
}

TEST_CASE("view: the log-silence notices and a missing CK3 folder") {
    ViewInputs inputs;
    inputs.configured = true;
    inputs.notice = advisor::Notice::RestartCk3;
    CHECK(hasBanner(buildChatView(inputs), text::kRestartCk3));
    inputs.notice = advisor::Notice::SilentSoft;
    CHECK(hasBanner(buildChatView(inputs), text::kSilentSoft));
    inputs.notice = advisor::Notice::Ck3NotRunning;
    CHECK(hasBanner(buildChatView(inputs), text::kCk3NotRunning));
    inputs.notice = advisor::Notice::None;
    CHECK(buildChatView(inputs).banners.empty());

    inputs.gameFolderFound = false;
    ChatView view = buildChatView(inputs);
    // Windows and macOS: the folder is in Documents, and the text says so.
    CHECK((view.status == text::kFolderNotFoundDocuments));
    CHECK((view.statusTone == Tone::Warning));
    CHECK(view.emptyState.empty());
    inputs.ck3FolderInDocuments = false;
    view = buildChatView(inputs);
    CHECK((view.status == text::kFolderNotFound));
    CHECK((view.statusTone == Tone::Warning));
}

TEST_CASE("view: a key split by markers is redacted in what is shown and copied") {
    // The session redacts the raw answer, but these splits pass that check: the markers keep the
    // key's bytes apart, and removing them joins the key again.
    SecretRegistry registry;
    const std::string key = "sk-ant-api03-FAKEFAKEFAKEFAKEFAKE0123456789";
    REQUIRE(registry.add(key));
    const std::vector<std::string> answers = {
        "Your key is sk-ant-**api03-FAKEFAKEFAKEFAKEFAKE0123456789** ok",
        "Your key is sk-ant-api03-FAKE**FAKEFAKEFAKEFAKE0123456789** ok",
        "Your key is sk-ant-`api03-FAKEFAKEFAKEFAKEFAKE0123456789` ok",
        "Your key is sk-ant-api03-*FAKEFAKEFAKEFAKEFAKE0123456789* ok",
        "Your key is [sk-ant-api03-FAKE](x)FAKEFAKEFAKEFAKE0123456789 ok",
        "Your key is sk-ant-api03\\-FAKEFAKEFAKEFAKEFAKE0123456789 ok",
        "**Title**\nYour key is sk-ant-~~api03-FAKEFAKEFAKEFAKEFAKE0123456789~~ ok",
    };
    for (const std::string& raw : answers) {
        CAPTURE(raw);
        REQUIRE((registry.redactFinal(raw) == raw));  // the session's check lets it through
        const AnswerText shownText = shownAnswer(raw, true, &registry);
        const std::string copied = plainText(shownText);
        CHECK(copied.find("FAKEFAKEFAKE") == std::string::npos);
        CHECK(copied.find(kRedactedMarker) != std::string::npos);
        for (const AnswerLine& line : shownText.lines) {
            for (const Run& run : line.runs) {
                CHECK(run.text.find("FAKEFAKEFAKE") == std::string::npos);
                CHECK_FALSE(run.bold);  // a redacted answer is shown as plain lines
            }
        }
    }
    // While it arrives, the start of the key at the end is not shown either.
    const std::string arriving =
        plainText(shownAnswer("Your key is sk-ant-**api03-FAKEFAKE", false, &registry));
    CHECK(arriving.find("api03-FAKE") == std::string::npos);
    // An answer without a secret keeps its bold.
    const AnswerText clean = shownAnswer("Your **heir** is safe.", true, &registry);
    CHECK(clean.lines.at(0).runs.at(1).bold);

    // The same through the window's view of a real session.
    Rig rig({answer({"Use sk-ant-**api03-FAKE", "FAKEFAKEFAKEFAKE0123456789** now."})});
    REQUIRE(rig.registry.add(key));
    REQUIRE((rig.controller.onUserAsk("What is my key?") == advisor::AskOutcome::Started));
    rig.step();
    CHECK(shown(rig.view().items.at(1)).find("api03-FAKE") == std::string::npos);
    rig.finish();
    const std::string ended = shown(rig.view().items.at(1));
    CHECK(ended.find("FAKEFAKEFAKE") == std::string::npos);
    CHECK(ended.find(kRedactedMarker) != std::string::npos);
}

TEST_CASE("view: blank drafts do not count") {
    CHECK(isBlankText(""));
    CHECK(isBlankText(" \n\t\r\n"));
    CHECK_FALSE(isBlankText(" a "));
}

TEST_CASE("log path: --log first, then the first existing candidate, else none") {
    const std::vector<std::filesystem::path> dirs = {"first", "second", "third"};
    std::set<std::filesystem::path> existing;
    const ExistsFn exists = [&](const std::filesystem::path& path) {
        return existing.contains(path);
    };
    CHECK((logFileIn("dir") == std::filesystem::path("dir") / "logs" / "debug.log"));

    LogPathChoice choice = chooseLogPath("", dirs, exists);
    CHECK(choice.path.empty());
    CHECK_FALSE(choice.fromFlag);

    existing.insert(logFileIn("third"));
    choice = chooseLogPath("", dirs, exists);
    CHECK((choice.path == logFileIn("third")));
    existing.insert(logFileIn("second"));
    CHECK((chooseLogPath("", dirs, exists).path == logFileIn("second")));
    // A folder without the log does not count.
    existing.insert("first");
    CHECK((chooseLogPath("", dirs, exists).path == logFileIn("second")));

    // --log wins, whether or not the file exists yet.
    choice = chooseLogPath("my/debug.log", dirs, exists);
    CHECK((choice.path == std::filesystem::path("my/debug.log")));
    CHECK(choice.fromFlag);
    CHECK((chooseLogPath("x.log", {}, {}).path == std::filesystem::path("x.log")));
    // No candidates, or nothing to ask: none.
    CHECK(chooseLogPath("", {}, exists).path.empty());
    CHECK(chooseLogPath("", dirs, {}).path.empty());
}

TEST_CASE("canned conversation: the smoke script plays through the real controller") {
    Rig rig(canned::smokeScript());
    rig.frame(canned::firstFrame());
    REQUIRE((rig.controller.onUserAsk(canned::kFirstQuestion) == advisor::AskOutcome::Started));
    rig.finish();
    ChatView view = rig.view();
    REQUIRE(view.items.size() == 2);
    const ViewItem& first = view.items[1];
    CHECK((first.state == AnswerState::Complete));
    // Every kind of markup is there: a heading (stripped), bold, three numbered lines, a bullet,
    // code without backticks.
    const std::string plain = shown(first);
    CHECK((plain.rfind("Your realm today\n", 0) == 0));
    CHECK(plain.find("1. Win over") != std::string::npos);
    CHECK(plain.find("2. Watch") != std::string::npos);
    CHECK(plain.find("3. Keep 100 gold") != std::string::npos);
    CHECK(plain.find(std::string(kBulletMarker) + "Your claim") != std::string::npos);
    CHECK(plain.find("the Vassals window") != std::string::npos);
    CHECK(plain.find('*') == std::string::npos);
    CHECK(plain.find('`') == std::string::npos);
    CHECK(plain.find('#') == std::string::npos);
    int numbered = 0;
    bool anyBold = false;
    for (const AnswerLine& line : first.answer.lines) {
        numbered += line.marker == "1. " || line.marker == "2. " || line.marker == "3. " ? 1 : 0;
        for (const Run& run : line.runs) {
            anyBold = anyBold || run.bold;
        }
    }
    CHECK(numbered == 3);
    CHECK(anyBold);

    rig.frame(canned::secondFrame());
    REQUIRE((rig.controller.onUserAsk(canned::kSecondQuestion) == advisor::AskOutcome::Started));
    rig.finish();
    view = rig.view();
    REQUIRE(view.items.size() == 5);
    CHECK((view.items[2].kind == ItemKind::Divider));
    CHECK((view.items[4].state == AnswerState::Failed));
    CHECK(view.items[4].offerRetry);
    CHECK(rig.provider->requests() == 2);

    // The pieces never split a character and join to the text.
    std::string joined;
    for (const std::string& piece : canned::splitIntoPieces(canned::firstAnswer(), 9)) {
        CHECK(piece.size() <= 9);
        CHECK_FALSE(piece.empty());
        CHECK((static_cast<unsigned char>(piece.front()) & 0xC0U) != 0x80U);
        joined += piece;
    }
    CHECK((joined == canned::firstAnswer()));
    // A request beyond the script fails instead of hanging.
    REQUIRE((rig.controller.onRetry() == advisor::AskOutcome::Started));
    rig.finish();
    CHECK((rig.controller.session().state() == ChatState::Error));
}
