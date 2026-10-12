// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "advisor/chat_error_text.h"
#include "advisor/chat_session.h"
#include "advisor/persona.h"
#include "advisor_test_support.h"
#include "bridge/text_hygiene.h"
#include "bridge_test_support.h"
#include "core/redact.h"
#include "core/ui_message.h"

using namespace curia;
using namespace curia::advisor;
using namespace curia::advisor::test;
using providers::ChatError;
using providers::StreamStatus;
using providers::TokenKind;
using providers::Usage;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

class Recorder final : public ChatObserver {
public:
    void onChatState(ChatState state) override { states.push_back(state); }
    void onTranscriptChanged(std::size_t index) override { changed.push_back(index); }

    [[nodiscard]] std::size_t terminalCount() const {
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
    std::vector<std::size_t> changed;
};

struct Rig {
    Rig() : session(mailbox, makeOptions()) {
        session.setProvider(&provider);
        session.setObserver(&recorder);
    }

    ChatSession::Options makeOptions() {
        ChatSession::Options options;
        options.registry = &registry;
        options.clock = clock.fn();
        return options;
    }

    static AskContext context() {
        AskContext ctx;
        ctx.settings.model = "mock-model-1";
        ctx.snapshotText = "SNAPSHOT TEXT";
        return ctx;
    }

    AskOutcome ask(std::string_view text) { return session.ask(text, context()); }

    // Hands every queued message to the session, as the UI loop does.
    std::size_t pump() {
        const std::vector<UiMessage> messages = mailbox->drain();
        for (const UiMessage& message : messages) {
            session.onMessage(message);
        }
        return messages.size();
    }

    // The handlers of the n-th request (the provider's side).
    providers::StreamHandlers handlers(std::size_t index) const {
        return provider.call(index).handlers;
    }

    FakeClock clock;
    std::atomic<int> wakes{0};
    std::shared_ptr<UiMailbox> mailbox = makeUiMailbox([this] { ++wakes; });
    SecretRegistry registry;
    FakeProvider provider;
    Recorder recorder;
    ChatSession session;
};

Usage usageWith(std::string stop = "end_turn") {
    Usage usage;
    usage.input = 100;
    usage.output = 20;
    usage.cacheRead = 80;
    usage.cacheWrite = 10;
    usage.stopReason = std::move(stop);
    return usage;
}

ChatError errorWith(ChatError::Code code, std::string message = "server said no") {
    ChatError error;
    error.code = code;
    error.message = std::move(message);
    return error;
}

}  // namespace

TEST_CASE("session: ask starts one request with the persona, snapshot and question") {
    Rig rig;
    CHECK((rig.session.state() == ChatState::Idle));
    REQUIRE((rig.ask("  Is my heir safe?  ") == AskOutcome::Started));
    CHECK((rig.session.state() == ChatState::Waiting));
    CHECK(rig.session.inFlight());

    REQUIRE(rig.provider.callCount() == 1);
    const auto call = rig.provider.call(0);
    CHECK((call.request.model == "mock-model-1"));
    CHECK((call.request.systemPersona == persona()));
    CHECK((call.request.snapshotText == "SNAPSHOT TEXT"));
    REQUIRE(call.request.conversation.size() == 1);
    CHECK((call.request.conversation[0].text == "Is my heir safe?"));

    const auto& transcript = rig.session.transcript();
    REQUIRE(transcript.size() == 2);
    CHECK((transcript[0].kind == EntryKind::User));
    CHECK((transcript[0].text == "Is my heir safe?"));
    CHECK((transcript[1].kind == EntryKind::Assistant));
    CHECK((transcript[1].end == AnswerEnd::Pending));
    CHECK(rig.session.generation() == 1);
    CHECK(rig.session.lastRequest().personaBytes == persona().size());
    CHECK(rig.session.lastRequest().snapshotBytes == 13);
}

TEST_CASE("session: tokens stream, then done") {
    Rig rig;
    rig.clock.advance(std::chrono::seconds(1));
    REQUIRE((rig.ask("Q") == AskOutcome::Started));
    const auto handlers = rig.handlers(0);

    rig.clock.advance(std::chrono::seconds(2));
    handlers.onToken(TokenKind::Text, "Hello ");
    handlers.onToken(TokenKind::Text, "there.");
    CHECK(rig.pump() == 1);  // the two tokens were joined in the mailbox
    CHECK((rig.session.state() == ChatState::Streaming));
    CHECK((rig.session.transcript()[1].text == "Hello there."));
    CHECK(rig.session.transcript()[1].firstTokenMs == 2000);

    rig.clock.advance(std::chrono::seconds(3));
    handlers.onToken(TokenKind::Text, " More.");
    handlers.onDone(usageWith());
    rig.pump();
    CHECK((rig.session.state() == ChatState::Done));
    const TranscriptEntry& answer = rig.session.transcript()[1];
    CHECK((answer.text == "Hello there. More."));
    CHECK((answer.end == AnswerEnd::Complete));
    CHECK_FALSE(answer.hitTokenLimit);
    CHECK(answer.usage.output == 20);
    CHECK(answer.usage.cacheRead == 80);
    CHECK(answer.totalMs == 5000);
    CHECK(rig.recorder.terminalCount() == 1);
    CHECK((rig.recorder.states ==
           std::vector<ChatState>{ChatState::Waiting, ChatState::Streaming, ChatState::Done}));
    CHECK_FALSE(rig.session.inFlight());
}

TEST_CASE("session: stopping at the token limit is a normal end with a flag") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "Cut of");
    handlers.onDone(usageWith("max_tokens"));
    rig.pump();
    CHECK((rig.session.state() == ChatState::Done));
    CHECK(rig.session.transcript()[1].hitTokenLimit);
    CHECK((rig.session.transcript()[1].end == AnswerEnd::Complete));
}

TEST_CASE("session: refusals before a request starts") {
    Rig rig;
    CHECK((rig.ask("") == AskOutcome::EmptyText));
    CHECK((rig.ask("  \n\t ") == AskOutcome::EmptyText));
    CHECK(rig.provider.callCount() == 0);
    CHECK(rig.session.transcript().empty());

    REQUIRE((rig.ask("First") == AskOutcome::Started));
    CHECK((rig.ask("Second") == AskOutcome::Busy));
    CHECK(rig.provider.callCount() == 1);
    CHECK(rig.session.transcript().size() == 2);

    Rig none;
    none.session.setProvider(nullptr);
    CHECK((none.ask("Q") == AskOutcome::NoProvider));
    CHECK(none.session.transcript().empty());
}

TEST_CASE("session: cancel keeps the partial text and marks it stopped") {
    Rig rig;
    rig.ask("Q");
    const auto call = rig.provider.call(0);
    call.handlers.onToken(TokenKind::Text, "Partial answer");
    rig.pump();
    CHECK(rig.session.cancel());
    CHECK((rig.session.state() == ChatState::Cancelled));
    CHECK(call.cancel.cancelled());
    const TranscriptEntry& answer = rig.session.transcript()[1];
    CHECK((answer.text == "Partial answer"));
    CHECK((answer.end == AnswerEnd::Stopped));
    CHECK(rig.recorder.terminalCount() == 1);
    CHECK_FALSE(rig.session.cancel());  // nothing left to stop
}

TEST_CASE("session: late callbacks of a cancelled request change nothing") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "Before. ");
    rig.pump();
    rig.session.cancel();

    // Tokens, a status, a done and an error still arrive from the provider's thread.
    handlers.onToken(TokenKind::Text, "LATE");
    handlers.onStatus(StreamStatus::ThinkingStarted);
    handlers.onDone(usageWith());
    handlers.onError(errorWith(ChatError::Code::Network));
    rig.pump();
    const TranscriptEntry& answer = rig.session.transcript()[1];
    CHECK((answer.text == "Before. "));
    CHECK((answer.end == AnswerEnd::Stopped));
    CHECK((rig.session.state() == ChatState::Cancelled));
    CHECK(rig.recorder.terminalCount() == 1);
    CHECK_FALSE(rig.session.thinking());
}

TEST_CASE("session: cancel with tokens still in the mailbox ignores them") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "queued but not drained");
    rig.session.cancel();  // before the UI thread drained the token
    rig.pump();
    CHECK(rig.session.transcript()[1].text.empty());
    CHECK((rig.session.transcript()[1].end == AnswerEnd::Stopped));
}

TEST_CASE("session: cancel after done is a no-op, done after cancel is ignored") {
    Rig rig;
    rig.ask("Q");
    auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "A");
    handlers.onDone(usageWith());
    rig.pump();
    CHECK_FALSE(rig.session.cancel());
    CHECK((rig.session.state() == ChatState::Done));
    CHECK((rig.session.transcript()[1].end == AnswerEnd::Complete));

    rig.ask("Q2");
    handlers = rig.handlers(1);
    rig.session.cancel();
    handlers.onDone(usageWith());
    rig.pump();
    CHECK((rig.session.state() == ChatState::Cancelled));
    CHECK((rig.session.transcript()[3].end == AnswerEnd::Stopped));
}

TEST_CASE("session: a new ask supersedes the old request, whose callbacks are then ignored") {
    Rig rig;
    rig.ask("First");
    const auto first = rig.handlers(0);
    rig.session.cancel();
    rig.ask("Second");
    REQUIRE(rig.provider.callCount() == 2);
    const auto second = rig.handlers(1);
    CHECK(rig.session.generation() == 3);  // ask, cancel, ask

    first.onToken(TokenKind::Text, "from the old request");
    first.onDone(usageWith());
    // The real provider ends a cancelled request with onError(Cancelled); other late errors and
    // status changes of the old request must not touch the new one either.
    first.onError(errorWith(ChatError::Code::Cancelled));
    first.onError(errorWith(ChatError::Code::Network));
    first.onStatus(StreamStatus::ThinkingStarted);
    second.onToken(TokenKind::Text, "from the new one ");
    rig.pump();
    // The last word is held back while streaming; the blank lets the whole text show.
    CHECK((rig.session.transcript()[3].text == "from the new one "));
    CHECK((rig.session.state() == ChatState::Streaming));
    CHECK_FALSE(rig.session.thinking());
    CHECK((rig.session.transcript()[3].end == AnswerEnd::Pending));
    CHECK(rig.recorder.terminalCount() == 1);  // only the stop of the first ask
    second.onDone(usageWith());
    rig.pump();
    CHECK((rig.session.state() == ChatState::Done));
    CHECK((rig.session.transcript()[3].text == "from the new one "));
}

TEST_CASE("session: a stale error or status of a superseded request leaves a waiting one alone") {
    Rig rig;
    rig.ask("First");
    const auto first = rig.handlers(0);
    rig.session.cancel();
    rig.ask("Second");
    first.onError(errorWith(ChatError::Code::Cancelled));
    first.onStatus(StreamStatus::ThinkingStarted);
    first.onError(errorWith(ChatError::Code::Auth));
    rig.pump();
    CHECK((rig.session.state() == ChatState::Waiting));
    CHECK_FALSE(rig.session.thinking());
    CHECK((rig.session.transcript()[3].end == AnswerEnd::Pending));
    CHECK_FALSE(rig.session.transcript()[3].error.has_value());
    CHECK(rig.recorder.terminalCount() == 1);
}

TEST_CASE("session: exactly one terminal state per ask, whatever the provider does") {
    for (int variant = 0; variant < 5; ++variant) {
        CAPTURE(variant);
        Rig rig;
        rig.ask("Q");
        const auto handlers = rig.handlers(0);
        handlers.onToken(TokenKind::Text, "x");
        switch (variant) {
        case 0:
            handlers.onDone(usageWith());
            handlers.onDone(usageWith());
            break;
        case 1:
            handlers.onDone(usageWith());
            handlers.onError(errorWith(ChatError::Code::Network));
            break;
        case 2:
            handlers.onError(errorWith(ChatError::Code::Network));
            handlers.onDone(usageWith());
            break;
        case 3:
            handlers.onError(errorWith(ChatError::Code::Network));
            handlers.onError(errorWith(ChatError::Code::Auth));
            break;
        default:
            handlers.onError(errorWith(ChatError::Code::Cancelled));
            handlers.onDone(usageWith());
            break;
        }
        rig.pump();
        CHECK(rig.recorder.terminalCount() == 1);
        // And the next ask works.
        CHECK((rig.ask("again") == AskOutcome::Started));
    }
}

TEST_CASE("session: an error keeps the partial text and speaks plainly") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "Start of an ans");
    ChatError error = errorWith(ChatError::Code::RateLimit, "slow down");
    error.retryable = true;
    error.httpStatus = 429;
    error.retryAfterSeconds = 30;
    error.requestId = "req_mock0001";
    error.partialText = "Start of an answer that was longer";
    handlers.onError(error);
    rig.pump();
    CHECK((rig.session.state() == ChatState::Error));
    const TranscriptEntry& answer = rig.session.transcript()[1];
    CHECK((answer.end == AnswerEnd::Failed));
    // The provider's partial text is the whole text received: the session adopts it.
    CHECK((answer.text == "Start of an answer that was longer"));
    const ErrorInfo& info = curia::bridge::test::must(answer.error);
    CHECK(contains(info.summary, "too many requests"));
    CHECK(contains(info.summary, "30 seconds"));
    CHECK((info.detail == "slow down"));
    CHECK(info.retryable);
    CHECK(info.httpStatus == 429);
    CHECK((info.requestId == "req_mock0001"));
}

TEST_CASE("session: a cancelled error from the provider is a stop, not a failure") {
    Rig rig;
    rig.ask("Q");
    rig.handlers(0).onError(errorWith(ChatError::Code::Cancelled, "cancelled"));
    rig.pump();
    CHECK((rig.session.state() == ChatState::Cancelled));
    CHECK((rig.session.transcript()[1].end == AnswerEnd::Stopped));
    CHECK_FALSE(rig.session.transcript()[1].error.has_value());
}

TEST_CASE("session: retry answers the last question again, in place") {
    Rig rig;
    rig.ask("Q1");
    rig.handlers(0).onToken(TokenKind::Text, "A1");
    rig.handlers(0).onDone(usageWith());
    rig.pump();
    rig.ask("Q2");
    rig.handlers(1).onToken(TokenKind::Text, "half");
    rig.handlers(1).onError(errorWith(ChatError::Code::Network));
    rig.pump();
    REQUIRE((rig.session.state() == ChatState::Error));

    REQUIRE((rig.session.retry(rig.context()) == AskOutcome::Started));
    REQUIRE(rig.provider.callCount() == 3);
    const auto call = rig.provider.call(2);
    // The same question, with the first pair as history, and no duplicate user row.
    REQUIRE(call.request.conversation.size() == 3);
    CHECK((call.request.conversation[2].text == "Q2"));
    CHECK(rig.session.transcript().size() == 4);
    CHECK((rig.session.transcript()[3].end == AnswerEnd::Pending));
    CHECK(rig.session.transcript()[3].text.empty());
    CHECK_FALSE(rig.session.transcript()[3].error.has_value());
    CHECK((rig.session.state() == ChatState::Waiting));

    rig.handlers(2).onToken(TokenKind::Text, "good answer");
    rig.handlers(2).onDone(usageWith());
    rig.pump();
    CHECK((rig.session.transcript()[3].text == "good answer"));
    CHECK((rig.session.state() == ChatState::Done));
}

TEST_CASE("session: retry after stop, refusals of retry") {
    Rig rig;
    CHECK((rig.session.retry(rig.context()) == AskOutcome::NothingToRetry));
    rig.ask("Q1");
    CHECK((rig.session.retry(rig.context()) == AskOutcome::Busy));
    rig.session.cancel();
    CHECK((rig.session.retry(rig.context()) == AskOutcome::Started));
    CHECK(rig.provider.callCount() == 2);

    Rig none;
    none.session.setProvider(nullptr);
    CHECK((none.session.retry(none.context()) == AskOutcome::NoProvider));
}

TEST_CASE("session: dividers keep the history and a second one replaces the first") {
    Rig rig;
    rig.ask("Q1");
    rig.handlers(0).onToken(TokenKind::Text, "A1");
    rig.handlers(0).onDone(usageWith());
    rig.pump();
    rig.session.addDivider("New snapshot, game date 1 January 1067");
    rig.session.addDivider("New snapshot, game date 1 February 1067");
    REQUIRE(rig.session.transcript().size() == 3);
    CHECK((rig.session.transcript()[2].kind == EntryKind::Divider));
    CHECK(contains(rig.session.transcript()[2].text, "1 February"));
    CHECK((rig.session.transcript()[0].text == "Q1"));  // nothing cleared

    rig.ask("Q2");
    const auto call = rig.provider.call(1);
    // The divider is not sent; the history is.
    REQUIRE(call.request.conversation.size() == 3);
    CHECK((call.request.conversation[0].text == "Q1"));
    CHECK((call.request.conversation[1].text == "A1"));
    CHECK((call.request.conversation[2].text == "Q2"));
}

TEST_CASE("session: a divider added while streaming does not disturb the answer") {
    Rig rig;
    rig.ask("Q1");
    rig.handlers(0).onToken(TokenKind::Text, "part one ");
    rig.pump();
    rig.session.addDivider("New snapshot");
    rig.handlers(0).onToken(TokenKind::Text, "part two");
    rig.handlers(0).onDone(usageWith());
    rig.pump();
    CHECK((rig.session.transcript()[1].text == "part one part two"));
    CHECK((rig.session.transcript()[2].kind == EntryKind::Divider));
    CHECK((rig.session.state() == ChatState::Done));
}

TEST_CASE("session: thinking keeps the spinner on until text starts") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    CHECK_FALSE(rig.session.thinking());
    handlers.onStatus(StreamStatus::ThinkingStarted);
    rig.pump();
    CHECK(rig.session.thinking());
    CHECK((rig.session.state() == ChatState::Waiting));
    handlers.onToken(TokenKind::Thinking, "private reasoning");
    rig.pump();
    CHECK(rig.session.transcript()[1].text.empty());  // reasoning is not shown
    handlers.onToken(TokenKind::Text, "Answer");
    rig.pump();
    CHECK_FALSE(rig.session.thinking());
    CHECK((rig.session.state() == ChatState::Streaming));
    // The last word is held back while streaming (see the hold-back test); it shows at the end.
    handlers.onDone(usageWith());
    rig.pump();
    CHECK((rig.session.transcript()[1].text == "Answer"));
}

TEST_CASE("session: a key split over two tokens is redacted in the joined text") {
    Rig rig;
    const std::string key = "sk-ant-FAKE-test-key-0000";
    REQUIRE(rig.registry.add(key));
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "The key is sk-ant-FAKE-te");
    rig.pump();
    handlers.onToken(TokenKind::Text, "st-key-0000 ok");
    rig.pump();
    const std::string& text = rig.session.transcript()[1].text;
    CHECK_FALSE(contains(text, "FAKE-test"));
    CHECK_FALSE(contains(text, "key-0000"));
    CHECK(contains(text, std::string(kRedactedMarker)));
    CHECK(text.rfind("The key is ", 0) == 0);
    CHECK(text.size() >= std::string("The key is ").size() + kRedactedMarker.size());
    // A key that arrives in one token is redacted too, in the error's partial text as well.
    ChatError error = errorWith(ChatError::Code::Network, "leak " + key);
    error.partialText = "partial " + key;
    handlers.onError(error);
    rig.pump();
    CHECK_FALSE(contains(rig.session.transcript()[1].text, "FAKE-test"));
    CHECK_FALSE(
        contains(curia::bridge::test::must(rig.session.transcript()[1].error).detail, "FAKE-test"));
}

TEST_CASE("session: a key split after its 20th character never shows in the joined text") {
    const std::string full = "sk-ant-api03-FAKEFAKEFAKEFAKEFAKE-test-0123456789ABCDEFGHIJ";
    const std::string head = full.substr(0, 33);  // the pattern rule already fires on the head
    const std::string tail = full.substr(33);
    for (const bool registered : {true, false}) {
        CAPTURE(registered);
        Rig rig;
        if (registered) {
            REQUIRE(rig.registry.add(full));
        }
        rig.ask("Q");
        const auto handlers = rig.handlers(0);
        handlers.onToken(TokenKind::Text, "The key is " + head);
        rig.pump();
        CHECK_FALSE(contains(rig.session.transcript()[1].text, "FAKEFAKE"));
        CHECK_FALSE(contains(rig.session.transcript()[1].text, "api03"));
        handlers.onToken(TokenKind::Text, tail + " ok");
        rig.pump();
        const std::string& text = rig.session.transcript()[1].text;
        CHECK_FALSE(contains(text, "FAKEFAKE"));
        CHECK_FALSE(contains(text, "0123456789"));
        CHECK_FALSE(contains(text, "test-"));
        CHECK_FALSE(contains(text, "ABCDEFGHIJ"));
        CHECK((text == "The key is [redacted] "));  // "ok" is the held-back last word
        handlers.onDone(usageWith());
        rig.pump();
        CHECK((rig.session.transcript()[1].text == "The key is [redacted] ok"));
    }
}

TEST_CASE("session: a registered key with odd characters, split in three, is hidden") {
    const std::string key = "tok+en/with.odd=chars-0123456789";
    Rig rig;
    REQUIRE(rig.registry.add(key));
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "See tok+en/wi");
    handlers.onToken(TokenKind::Text, "th.odd=cha");
    handlers.onToken(TokenKind::Text, "rs-0123456789 bye");
    rig.pump();
    const std::string& text = rig.session.transcript()[1].text;
    CHECK_FALSE(contains(text, "odd="));
    CHECK_FALSE(contains(text, "0123456789"));
    CHECK(contains(text, std::string(kRedactedMarker)));
}

TEST_CASE("session: the last word is held back while streaming and shown at the end") {
    Rig rig;
    rig.ask("Q");
    const auto handlers = rig.handlers(0);
    handlers.onToken(TokenKind::Text, "Check your he");
    rig.pump();
    CHECK((rig.session.transcript()[1].text == "Check your "));
    CHECK((rig.session.state() == ChatState::Streaming));
    handlers.onToken(TokenKind::Text, "ir now");
    rig.pump();
    CHECK((rig.session.transcript()[1].text == "Check your heir "));
    handlers.onDone(usageWith());
    rig.pump();
    CHECK((rig.session.transcript()[1].text == "Check your heir now"));
    // A stop shows the held word too.
    rig.ask("Q2");
    rig.handlers(1).onToken(TokenKind::Text, "Half an ans");
    rig.pump();
    CHECK((rig.session.transcript()[3].text == "Half an "));
    rig.session.cancel();
    CHECK((rig.session.transcript()[3].text == "Half an ans"));
}

TEST_CASE("session: an answer with no text is an error, not a blank answer") {
    Rig rig;
    rig.ask("Q");
    rig.handlers(0).onDone(usageWith());
    rig.pump();
    CHECK((rig.session.state() == ChatState::Error));
    const TranscriptEntry& answer = rig.session.transcript()[1];
    CHECK((answer.end == AnswerEnd::Failed));
    CHECK(answer.text.empty());
    const ErrorInfo& info = curia::bridge::test::must(answer.error);
    CHECK(contains(info.summary, "empty answer"));
    CHECK(info.retryable);
    CHECK(rig.recorder.terminalCount() == 1);
    // And it can be asked again.
    CHECK((rig.session.retry(Rig::context()) == AskOutcome::Started));

    // With the token limit hit before any text (for example all spent on thinking), say that.
    Rig limited;
    limited.ask("Q");
    limited.handlers(0).onStatus(StreamStatus::ThinkingStarted);
    limited.handlers(0).onDone(usageWith("max_tokens"));
    limited.pump();
    CHECK((limited.session.state() == ChatState::Error));
    CHECK(contains(curia::bridge::test::must(limited.session.transcript()[1].error).summary,
                   "ran out of room"));
    CHECK_FALSE(limited.session.thinking());
}

TEST_CASE("session: the question is redacted before it is stored or sent") {
    Rig rig;
    const std::string key = "sk-ant-FAKE-test-key-0000";
    REQUIRE(rig.registry.add(key));
    rig.ask("Why does " + key + " not work?");
    CHECK_FALSE(contains(rig.session.transcript()[0].text, "FAKE-test"));
    CHECK_FALSE(contains(rig.provider.call(0).request.conversation[0].text, "FAKE-test"));
    CHECK(contains(rig.session.transcript()[0].text, std::string(kRedactedMarker)));
}

TEST_CASE("session: the provider's thread talks to the mailbox only") {
    Rig rig;
    FakeProvider::Script script;
    script.onThread = true;
    for (int i = 0; i < 200; ++i) {
        script.tokens.push_back("w" + std::to_string(i) + " ");
    }
    script.usage = usageWith();
    rig.provider.setScript(script);
    REQUIRE((rig.ask("Q") == AskOutcome::Started));
    REQUIRE(waitUntil([&] {
        rig.pump();
        return !rig.session.inFlight();
    }));
    std::string expected;
    for (int i = 0; i < 200; ++i) {
        expected += "w" + std::to_string(i) + " ";
    }
    CHECK((rig.session.transcript()[1].text == expected));
    CHECK((rig.session.state() == ChatState::Done));
    CHECK(rig.recorder.terminalCount() == 1);
    CHECK(rig.wakes.load() >= 1);
}

TEST_CASE("session: a provider that answers inline, during streamChat") {
    Rig rig;
    FakeProvider::Script script;
    script.tokens = {"Quick ", "answer."};
    script.usage = usageWith();
    rig.provider.setScript(script);
    REQUIRE((rig.ask("Q") == AskOutcome::Started));
    // Nothing touched the session yet: the handlers only posted.
    CHECK((rig.session.state() == ChatState::Waiting));
    rig.pump();
    CHECK((rig.session.state() == ChatState::Done));
    CHECK((rig.session.transcript()[1].text == "Quick answer."));
}

TEST_CASE("session: destroying it with a request in flight cancels the token") {
    FakeProvider provider;
    providers::CancelToken token;
    {
        FakeClock clock;
        auto mailbox = makeUiMailbox({});
        ChatSession::Options options;
        options.clock = clock.fn();
        const SecretRegistry registry;
        options.registry = &registry;
        ChatSession session(mailbox, options);
        session.setProvider(&provider);
        AskContext context;
        context.settings.model = "mock-model-1";
        session.ask("Q", context);
        token = provider.call(0).cancel;
        CHECK_FALSE(token.cancelled());
    }
    CHECK(token.cancelled());
}

// --- error wording ----------------------------------------------------------------------------

TEST_CASE("error text: one plain sentence per code, no server text in the summary") {
    struct Row {
        ChatError::Code code;
        const char* part;
    };
    const std::vector<Row> rows = {
        {ChatError::Code::Auth, "did not accept the API key"},
        {ChatError::Code::Network, "Could not reach the service"},
        {ChatError::Code::RateLimit, "too many requests"},
        {ChatError::Code::Overloaded, "busy right now"},
        {ChatError::Code::BadRequest, "model name"},
        {ChatError::Code::Policy, "declined to answer"},
        {ChatError::Code::Cancelled, "Stopped"},
        {ChatError::Code::Other, "Something went wrong"},
    };
    for (const Row& row : rows) {
        CAPTURE(toString(row.code));
        const ErrorInfo info = describeChatError(errorWith(row.code, "SERVER-WORDS"));
        CHECK(contains(info.summary, row.part));
        CHECK_FALSE(contains(info.summary, "SERVER-WORDS"));
        CHECK((info.code == row.code));
        CHECK((info.detail == "SERVER-WORDS"));
    }
}

TEST_CASE("error text: the wait, the redaction and the length of the detail") {
    ChatError limited = errorWith(ChatError::Code::RateLimit);
    CHECK(contains(describeChatError(limited).summary, "Try again in a moment."));
    limited.retryAfterSeconds = 1;
    CHECK(contains(describeChatError(limited).summary, "in 1 second."));
    limited.retryAfterSeconds = 90;
    CHECK(contains(describeChatError(limited).summary, "in 90 seconds."));
    limited.retryAfterSeconds = 0;
    CHECK(contains(describeChatError(limited).summary, "in a moment"));

    SecretRegistry registry;
    REQUIRE(registry.add("sk-ant-FAKE-test-key-0000"));
    ChatError leaky = errorWith(ChatError::Code::Auth, "bad key sk-ant-FAKE-test-key-0000 sorry");
    leaky.requestId = "req_sk-ant-FAKE-test-key-0000";
    const ErrorInfo info = describeChatError(leaky, &registry);
    CHECK_FALSE(contains(info.detail, "FAKE-test"));
    CHECK_FALSE(contains(info.requestId, "FAKE-test"));
    CHECK(contains(info.detail, std::string(kRedactedMarker)));

    const ChatError wordy = errorWith(ChatError::Code::Other, std::string(1000, 'e'));
    CHECK(describeChatError(wordy).detail.size() <= kMaxErrorDetailBytes);
    // The cut never splits a character.
    ChatError accents = errorWith(ChatError::Code::Other);
    accents.message.clear();
    for (int i = 0; i < 400; ++i) {
        accents.message += "\xC3\xA9";
    }
    const std::string cut = describeChatError(accents).detail;
    CHECK(cut.size() <= kMaxErrorDetailBytes);
    CHECK(cut.size() % 2 == 0);

    // A three-byte and a four-byte character that straddle the limit are dropped whole.
    for (const std::string& wide : {std::string("\xE2\x82\xAC"), std::string("\xF0\x9F\x98\x80")}) {
        for (std::size_t pad = kMaxErrorDetailBytes - wide.size(); pad <= kMaxErrorDetailBytes;
             ++pad) {
            ChatError straddle = errorWith(ChatError::Code::Other);
            straddle.message = std::string(pad, 'a') + wide + "tail";
            const std::string result = describeChatError(straddle).detail;
            CAPTURE(pad);
            CAPTURE(wide.size());
            CHECK(result.size() <= kMaxErrorDetailBytes);
            CHECK((curia::bridge::repairUtf8(result) == result));
        }
    }
}
