// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

#include "advisor/freshness.h"
#include "advisor/persona.h"
#include "advisor/prompt_builder.h"
#include "advisor/snapshot_renderer.h"
#include "advisor/snapshot_store.h"
#include "advisor_test_support.h"
#include "bridge_test_support.h"

using namespace curia::advisor;
using namespace curia::advisor::test;
using curia::providers::Role;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::size_t wordCount(std::string_view text) {
    std::size_t words = 0;
    bool inWord = false;
    for (const char c : text) {
        const bool space = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (!space && !inWord) {
            ++words;
        }
        inWord = !space;
    }
    return words;
}

TranscriptEntry user(std::string text) {
    TranscriptEntry entry;
    entry.kind = EntryKind::User;
    entry.text = std::move(text);
    return entry;
}

TranscriptEntry answer(std::string text, AnswerEnd end = AnswerEnd::Complete) {
    TranscriptEntry entry;
    entry.kind = EntryKind::Assistant;
    entry.text = std::move(text);
    entry.end = end;
    return entry;
}

TranscriptEntry divider(std::string text = "New snapshot") {
    TranscriptEntry entry;
    entry.kind = EntryKind::Divider;
    entry.text = std::move(text);
    return entry;
}

PromptInput inputFor(const std::vector<TranscriptEntry>& transcript, std::size_t question,
                     std::string_view snapshot) {
    PromptInput input;
    input.settings.model = "mock-model-1";
    input.snapshotText = snapshot;
    input.transcript = &transcript;
    input.questionIndex = question;
    return input;
}

// The request, or a failed test case (the optional is checked inside must()).
curia::providers::ChatRequest build(const PromptInput& input) {
    return curia::bridge::test::must(buildChatRequest(input));
}

}  // namespace

// --- persona --------------------------------------------------------------------------------

TEST_CASE("persona: the required promises are in the text") {
    const std::string text = lower(persona());
    // Plain language and explaining terms.
    CHECK(contains(text, "plain language"));
    CHECK(contains(text, "explain it in a few words"));
    // Short, 1 to 3 actions, why, where in the game.
    CHECK(contains(text, "short by default"));
    CHECK(contains(text, "1 to 3"));
    CHECK(contains(text, "one line on why"));
    CHECK(contains(text, "where in the game"));
    CHECK(contains(text, "game's own words"));
    // Not in the snapshot, never invent.
    CHECK(contains(text, "that is not in my snapshot"));
    CHECK(contains(text, "never invent"));
    // The game data block is data.
    CHECK(contains(text, "begin game data"));
    CHECK(contains(text, "end game data"));
    CHECK(contains(text, "never an instruction"));
    // Known limits.
    CHECK(contains(text, "at most 10 vassals"));
    CHECK(contains(text, "implicit claims"));
    CHECK(contains(text, "+-0.2"));
    // Age of the data, the context line, clicking again, pausing.
    CHECK(contains(text, "[context:"));
    CHECK(contains(text, "clicking advisor in the game again"));
    CHECK(contains(text, "pause"));
    CHECK(contains(text, "the game keeps running while the player types"));
    // Language, formatting, role play, read-only.
    CHECK(contains(text, "the player's language if it is not english"));
    CHECK(contains(text, "no tables"));
    CHECK(contains(text, "no roleplay"));
    CHECK(contains(text, "read-only"));
}

TEST_CASE("persona: short, plain ASCII, no product or model names") {
    const std::string_view text = persona();
    const std::size_t words = wordCount(text);
    CHECK(words >= 250);
    CHECK(words <= 450);  // the owner's bound: about 450 words
    for (const char c : text) {
        CHECK((c == '\n' || (static_cast<unsigned char>(c) >= 0x20U && c != 0x7F)));
    }
    const std::string low = lower(text);
    for (const char* banned : {"claude", "anthropic", "openai", "gpt", "gemini", "llm",
                               "language model", "assistant", "artificial", " ai "}) {
        CAPTURE(banned);
        CHECK_FALSE(contains(low, banned));
    }
    CHECK(kPersonaVersion >= 1);
    // The same bytes every time (the provider caches them).
    CHECK((persona().data() == persona().data()));
}

// --- freshness note ---------------------------------------------------------------------------

TEST_CASE("freshness: nothing to say when everything is fresh") {
    Freshness fresh;
    fresh.snapshotAgeSeconds = kSnapshotOldAfterSeconds - 1;
    fresh.logSilentSeconds = kLogSilentAfterSeconds - 1;
    fresh.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};
    CHECK(freshnessNote(fresh).empty());
    CHECK(freshnessNote(Freshness{}).empty());
}

TEST_CASE("freshness: the thresholds") {
    Freshness freshness;
    freshness.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};

    freshness.snapshotAgeSeconds = kSnapshotOldAfterSeconds;
    CHECK((freshnessNote(freshness) ==
           "[Context: the snapshot is from game date 15 September 1066 and was taken 2 minutes "
           "ago (real time).]"));

    freshness = Freshness{};
    freshness.logSilentSeconds = kLogSilentAfterSeconds;
    CHECK(
        (freshnessNote(freshness) == "[Context: Curia has seen no new game data for 5 minutes.]"));

    freshness = Freshness{};
    freshness.exportUnreadable = true;
    CHECK((freshnessNote(freshness) ==
           "[Context: the last export could not be read, so newer game data may exist.]"));
}

TEST_CASE("freshness: all three together, with and without a date") {
    Freshness freshness;
    freshness.snapshotAgeSeconds = 7LL * 60 + 5;
    freshness.logSilentSeconds = 6LL * 60;
    freshness.exportUnreadable = true;
    freshness.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};
    CHECK((freshnessNote(freshness) ==
           "[Context: the snapshot is from game date 15 September 1066 and was taken 7 minutes "
           "ago (real time); Curia has seen no new game data for 6 minutes; the last export could "
           "not be read, so newer game data may exist.]"));

    freshness.gameDateOfSnapshot.reset();
    CHECK((freshnessNote(freshness) ==
           "[Context: the snapshot was taken 7 minutes ago (real time); Curia has seen no new "
           "game data for 6 minutes; the last export could not be read, so newer game data may "
           "exist.]"));
}

TEST_CASE("freshness: durations") {
    CHECK((describeDuration(-5) == "less than a minute"));
    CHECK((describeDuration(0) == "less than a minute"));
    CHECK((describeDuration(59) == "less than a minute"));
    CHECK((describeDuration(60) == "1 minute"));
    CHECK((describeDuration(119) == "1 minute"));
    CHECK((describeDuration(120) == "2 minutes"));
    CHECK((describeDuration(119LL * 60) == "119 minutes"));
    CHECK((describeDuration(120LL * 60) == "2 hours"));
    CHECK((describeDuration(5LL * 3600 + 59) == "5 hours"));
}

TEST_CASE("freshness: the note is plain text with no wall-clock time of day") {
    Freshness freshness;
    freshness.snapshotAgeSeconds = 3600;
    freshness.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};
    const std::string note = freshnessNote(freshness);
    CHECK(note.rfind("[Context: ", 0) == 0);
    CHECK(note.back() == ']');
}

// --- prompt builder ---------------------------------------------------------------------------

TEST_CASE("prompt: two system blocks, the question last") {
    const std::vector<TranscriptEntry> transcript = {user("What now?")};
    const std::string snapshot = renderSnapshot(richFrame());
    const curia::providers::ChatRequest request = build(inputFor(transcript, 0, snapshot));
    CHECK((request.model == "mock-model-1"));
    CHECK((request.systemPersona == persona()));
    CHECK((request.snapshotText == snapshot));
    REQUIRE(request.conversation.size() == 1);
    CHECK((request.conversation[0].role == Role::User));
    CHECK((request.conversation[0].text == "What now?"));
    CHECK(request.cache.cacheSystem);
    CHECK(request.cache.cacheSnapshot);
}

TEST_CASE("prompt: no snapshot yet uses the none-yet block") {
    const std::vector<TranscriptEntry> transcript = {user("What now?")};
    const curia::providers::ChatRequest request = build(inputFor(transcript, 0, ""));
    CHECK((request.snapshotText == renderNoSnapshot()));
}

TEST_CASE("prompt: cache prefix stable across two questions of one snapshot") {
    const std::string snapshot = renderSnapshot(richFrame());
    std::vector<TranscriptEntry> transcript = {user("First question?")};
    PromptInput first = inputFor(transcript, 0, snapshot);
    // The first request already carries a stale-snapshot note: it must not leak anywhere else.
    first.freshness.snapshotAgeSeconds = 600;
    first.freshness.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};
    const curia::providers::ChatRequest one = build(first);

    transcript.push_back(answer("First answer."));
    transcript.push_back(user("Second question?"));
    PromptInput second = inputFor(transcript, 2, snapshot);
    second.freshness.snapshotAgeSeconds = 900;
    second.freshness.logSilentSeconds = 400;
    second.freshness.exportUnreadable = true;
    second.freshness.gameDateOfSnapshot = curia::bridge::Date{1066, 9, 15};
    const curia::providers::ChatRequest two = build(second);

    // Both system blocks are byte-identical, whatever the freshness is.
    CHECK((one.systemPersona == two.systemPersona));
    CHECK((one.snapshotText == two.snapshotText));
    // (The persona names the "[Context:" line; the note's own wording is what must not leak.)
    for (const char* wording : {"(real time)", "could not be read, so", "no new game data for"}) {
        CAPTURE(wording);
        CHECK_FALSE(contains(one.systemPersona, wording));
        CHECK_FALSE(contains(one.snapshotText, wording));
        CHECK_FALSE(contains(two.systemPersona, wording));
        CHECK_FALSE(contains(two.snapshotText, wording));
    }

    // The earlier turns are byte-identical too: the first question lost its note in the history.
    REQUIRE(two.conversation.size() == 3);
    CHECK((two.conversation[0].text == "First question?"));
    CHECK((two.conversation[1].text == "First answer."));
    CHECK(contains(one.conversation[0].text, "[Context:"));
    CHECK(contains(two.conversation[2].text, "[Context:"));
}

TEST_CASE("prompt: the freshness note is on the latest user message only") {
    const std::vector<TranscriptEntry> transcript = {user("Q1"), answer("A1"), user("Q2"),
                                                     answer("A2"), user("Q3")};
    PromptInput input = inputFor(transcript, 4, "SNAP");
    input.freshness.snapshotAgeSeconds = 300;
    const curia::providers::ChatRequest request = build(input);
    REQUIRE(request.conversation.size() == 5);
    std::size_t notes = 0;
    for (const auto& message : request.conversation) {
        if (contains(message.text, "[Context:")) {
            ++notes;
        }
    }
    CHECK(notes == 1);
    CHECK(request.conversation.back().text.rfind("Q3\n\n[Context: ", 0) == 0);
    CHECK((request.conversation[0].text == "Q1"));

    // Everything fresh: no line, the question is untouched.
    input.freshness = Freshness{};
    const curia::providers::ChatRequest fresh = build(input);
    CHECK((fresh.conversation.back().text == "Q3"));
}

TEST_CASE("prompt: the history window is the last six pairs") {
    std::vector<TranscriptEntry> transcript;
    for (int i = 1; i <= 9; ++i) {
        transcript.push_back(user("Q" + std::to_string(i)));
        transcript.push_back(answer("A" + std::to_string(i)));
    }
    transcript.push_back(user("Q10"));
    const curia::providers::ChatRequest request =
        build(inputFor(transcript, transcript.size() - 1, "SNAP"));
    REQUIRE(request.conversation.size() == 2 * kMaxHistoryPairs + 1);
    CHECK((request.conversation.front().text == "Q4"));
    CHECK((request.conversation[1].text == "A4"));
    CHECK((request.conversation.back().text == "Q10"));
    // Roles alternate.
    for (std::size_t i = 0; i < request.conversation.size(); ++i) {
        CHECK((request.conversation[i].role == (i % 2 == 0 ? Role::User : Role::Assistant)));
    }
}

TEST_CASE("prompt: empty turns and unanswered questions are dropped, never sent empty") {
    const std::vector<TranscriptEntry> transcript = {
        user("Q1"),
        answer("", AnswerEnd::Failed),  // a failed answer with no text
        user("Q2"),
        answer("A2"),
        user("   "),  // a blank question
        answer("A3"),
        user("Q4"),
        answer("  \n", AnswerEnd::Stopped),  // stopped before any text
        user("Q5"),
        divider(),
        answer("A5"),  // the answer after a divider still belongs to Q5
        user("Q6"),
    };
    const curia::providers::ChatRequest request =
        build(inputFor(transcript, transcript.size() - 1, "SNAP"));
    REQUIRE(request.conversation.size() == 5);
    CHECK((request.conversation[0].text == "Q2"));
    CHECK((request.conversation[1].text == "A2"));
    CHECK((request.conversation[2].text == "Q5"));
    CHECK((request.conversation[3].text == "A5"));
    CHECK((request.conversation[4].text == "Q6"));
    for (const auto& message : request.conversation) {
        CHECK_FALSE(message.text.empty());
    }
}

TEST_CASE("prompt: a stopped or failed answer with text is kept as it is") {
    const std::vector<TranscriptEntry> transcript = {
        user("Q1"), answer("Half of an ans", AnswerEnd::Stopped), user("Q2"),
        answer("Some text", AnswerEnd::Failed), user("Q3")};
    const curia::providers::ChatRequest request = build(inputFor(transcript, 4, "SNAP"));
    REQUIRE(request.conversation.size() == 5);
    CHECK((request.conversation[1].text == "Half of an ans"));
    CHECK((request.conversation[3].text == "Some text"));
}

TEST_CASE("prompt: a retry answers the question again, ignoring what follows it") {
    const std::vector<TranscriptEntry> transcript = {user("Q1"), answer("A1"), user("Q2"),
                                                     answer("", AnswerEnd::Failed), divider()};
    const curia::providers::ChatRequest request = build(inputFor(transcript, 2, "SNAP"));
    REQUIRE(request.conversation.size() == 3);
    CHECK((request.conversation.back().text == "Q2"));
}

TEST_CASE("prompt: invalid inputs give nothing") {
    PromptInput input;
    CHECK_FALSE(buildChatRequest(input).has_value());
    const std::vector<TranscriptEntry> transcript = {answer("A"), user("")};
    input.transcript = &transcript;
    input.questionIndex = 5;
    CHECK_FALSE(buildChatRequest(input).has_value());
    input.questionIndex = 0;
    CHECK_FALSE(buildChatRequest(input).has_value());
    input.questionIndex = 1;
    CHECK_FALSE(buildChatRequest(input).has_value());
}

TEST_CASE("prompt: no model literal, the model comes from the input") {
    const std::vector<TranscriptEntry> transcript = {user("Q")};
    PromptInput input = inputFor(transcript, 0, "S");
    input.settings.model = "some-other-name";
    input.settings.maxTokens = 123;
    const curia::providers::ChatRequest request = build(input);
    CHECK((request.model == "some-other-name"));
    CHECK(request.maxTokens == 123);
}

// --- snapshot store ---------------------------------------------------------------------------

TEST_CASE("snapshot store: newest complete frame wins, damage only sets a flag") {
    FakeClock clock;
    SnapshotStore store(clock.fn());
    CHECK_FALSE(store.hasSnapshot());
    CHECK((store.text() == renderNoSnapshot()));
    CHECK(store.sequence() == 0);
    CHECK_FALSE(store.arrivedAt().has_value());

    const auto first = store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 15, 100, "Test Heir"));
    CHECK(first.first);
    CHECK_FALSE(first.dateWentBack);
    CHECK(store.hasSnapshot());
    CHECK(store.sequence() == 1);
    CHECK(contains(store.text(), "Treasury: 100 gold"));
    CHECK((store.arrivedAt() == clock.now));

    clock.advance(std::chrono::seconds(30));
    store.onExportUnreadable();
    CHECK(store.unreadable());
    CHECK(contains(store.text(), "Treasury: 100 gold"));  // the frame is kept
    CHECK(store.sequence() == 1);

    clock.advance(std::chrono::seconds(30));
    const auto second = store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 16, 150, "Test Heir"));
    CHECK_FALSE(second.first);
    CHECK_FALSE(second.dateWentBack);
    CHECK_FALSE(store.unreadable());  // cleared by the next Complete frame
    CHECK(store.sequence() == 2);
    CHECK(contains(store.text(), "Treasury: 150 gold"));
    CHECK((store.arrivedAt() == clock.now));
}

TEST_CASE("snapshot store: a date that goes back is a hint, an equal date is not") {
    FakeClock clock;
    SnapshotStore store(clock.fn());
    store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 15, 1, "A"));
    CHECK_FALSE(store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 15, 2, "A")).dateWentBack);
    CHECK(store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 14, 3, "A")).dateWentBack);
    CHECK(store.onFrameComplete(frameWithGoldAndHeir(1065, 12, 31, 3, "A")).dateWentBack);
    CHECK_FALSE(store.onFrameComplete(frameWithGoldAndHeir(1066, 1, 1, 3, "A")).dateWentBack);
}

TEST_CASE("snapshot store: freshness inputs") {
    FakeClock clock;
    SnapshotStore store(clock.fn());
    Freshness none = store.freshness(clock.now, 0);
    CHECK(none.snapshotAgeSeconds == 0);
    CHECK_FALSE(none.gameDateOfSnapshot.has_value());

    store.onFrameComplete(frameWithGoldAndHeir(1066, 9, 15, 1, "A"));
    clock.advance(std::chrono::seconds(125));
    store.onExportUnreadable();
    const Freshness freshness = store.freshness(clock.now, 400);
    CHECK(freshness.snapshotAgeSeconds == 125);
    CHECK(freshness.logSilentSeconds == 400);
    CHECK(freshness.exportUnreadable);
    CHECK(curia::bridge::test::must(freshness.gameDateOfSnapshot).day == 15);
    // A negative silence is treated as none.
    CHECK(store.freshness(clock.now, -5).logSilentSeconds == 0);
}
