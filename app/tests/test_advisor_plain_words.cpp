// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/chat_error_text.h"
#include "advisor/chat_session.h"
#include "advisor/freshness.h"
#include "advisor/persona.h"
#include "advisor/plain_words.h"
#include "advisor/snapshot_renderer.h"
#include "advisor_test_support.h"
#include "core/app_controller.h"

using namespace curia::advisor;
using namespace curia::advisor::test;

// The plain-words rule (owner, 2026-10-17; plain_words.h has the list and the reason): every text
// that reaches the player, or that the model may repeat to the player, is free of Curia's
// internal words.

namespace {

void checkPlain(std::string_view what, std::string_view text) {
    CAPTURE(what);
    CAPTURE(text);
    const std::string_view found = findInternalWord(text);
    CHECK_MESSAGE(found.empty(), "internal word: " << std::string(found));
}

}  // namespace

TEST_CASE("plain words: the finder") {
    CHECK((findInternalWord("the snapshot") == "snapshot"));
    CHECK((findInternalWord("SNAPSHOTS") == "snapshot"));
    CHECK((findInternalWord("it was exported") == "export"));
    CHECK((findInternalWord("Game Data") == "game data"));
    CHECK((findInternalWord("no data here") == "data"));
    CHECK((findInternalWord("a frame") == "frame"));
    CHECK((findInternalWord("in debug.log") == "debug"));
    CHECK((findInternalWord("CURIA1|3|BEGIN") == "curia1"));
    CHECK((findInternalWord("the log said") == "log"));
    CHECK((findInternalWord("old logs") == "logs"));
    CHECK((findInternalWord("it was logged") == "logged"));
    CHECK((findInternalWord("Logging") == "logging"));
    CHECK((findInternalWord("a snap of it") == "snap"));
    CHECK((findInternalWord("the Markers") == "marker"));
    CHECK((findInternalWord("as JSON") == "json"));
    CHECK((findInternalWord("4096 tokens") == "token"));
    CHECK((findInternalWord("the block") == "block"));
    CHECK((findInternalWord("two blocks") == "blocks"));
    CHECK((findInternalWord("a FIN record") == "record"));
    CHECK((findInternalWord("records") == "records"));
    CHECK((findInternalWord("a field") == "field"));
    CHECK((findInternalWord("the fields") == "fields"));
    // Whole words only for data, log, snap, block, record and field.
    CHECK(findInternalWord("an update, a dialog, a catalog, logic, database").empty());
    CHECK(findInternalWord("blocked, a battlefield, recorded, snapped, a blockade").empty());
    CHECK(findInternalWord("").empty());
    CHECK(findInternalWord("Updated from the game: 3 October 1071").empty());
}

TEST_CASE("plain words: the persona") {
    checkPlain("persona", persona());
}

TEST_CASE("plain words: what the model reads about the realm") {
    // The hostile frame is left out: its names carry such words on purpose.
    checkPlain("rich", renderSnapshot(richFrame()));
    checkPlain("minimal", renderSnapshot(minimalFrame()));
    checkPlain("empty", renderSnapshot(emptyFrame()));
    checkPlain("none yet", renderNoSnapshot());
    curia::bridge::Frame shortened = minimalFrame();
    shortened.textShortened = true;
    checkPlain("shortened", renderSnapshot(shortened));
}

TEST_CASE("plain words: every freshness note") {
    for (const bool old : {false, true}) {
        for (const bool silent : {false, true}) {
            for (const bool unreadable : {false, true}) {
                for (const bool dated : {false, true}) {
                    Freshness freshness;
                    freshness.snapshotAgeSeconds = old ? 3 * kSnapshotOldAfterSeconds : 0;
                    freshness.logSilentSeconds = silent ? 2 * kLogSilentAfterSeconds : 0;
                    freshness.exportUnreadable = unreadable;
                    if (dated) {
                        freshness.gameDateOfSnapshot = curia::bridge::Date{1071, 10, 3};
                    }
                    CAPTURE(old);
                    CAPTURE(silent);
                    CAPTURE(unreadable);
                    CAPTURE(dated);
                    checkPlain("freshness note", freshnessNote(freshness));
                }
            }
        }
    }
}

TEST_CASE("plain words: the error summaries") {
    using Code = curia::providers::ChatError::Code;
    for (const Code code : {Code::Auth, Code::RateLimit, Code::Overloaded, Code::Network,
                            Code::Cancelled, Code::BadRequest, Code::Policy, Code::Other}) {
        for (const std::optional<int> wait : {std::optional<int>(), std::optional<int>(7)}) {
            curia::providers::ChatError error;
            error.code = code;
            error.retryAfterSeconds = wait;
            CAPTURE(curia::providers::toString(code));
            checkPlain("error summary", describeChatError(error).summary);
        }
    }
    checkPlain("empty answer", kEmptyAnswerText);
    checkPlain("no room for an answer", kNoRoomForAnswerText);
}

TEST_CASE("plain words: the divider and the not-configured text") {
    checkPlain("divider", curia::dividerLabel({1071, 10, 3}, false));
    checkPlain("divider after a load", curia::dividerLabel({1071, 10, 3}, true));
    CHECK((curia::dividerLabel({1071, 10, 3}, false) == "Updated from the game: 3 October 1071"));
    CHECK((curia::dividerLabel({1071, 10, 3}, true) ==
           "Updated from the game: 3 October 1071 (a save may have been loaded)"));
    checkPlain("not configured", curia::kDefaultNotConfigured);
}
