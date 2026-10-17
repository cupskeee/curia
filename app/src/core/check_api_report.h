// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "core/cli.h"
#include "providers/chat_types.h"

namespace curia {

// The owner's preflight (`curia --check-api`): the steps are run by the headless layer, this file
// holds what can be decided and worded without a network: the plain-language advice for each kind
// of failure, the verdict of the cache probe, and the layout of the report and its final summary
// line. Nothing here ever sees a key or a request body.

// One line of the report: "[ok] Connection: ..." / "[FAILED] ..." / "[skipped] ...".
struct CheckStep {
    enum class State { Ok, Failed, Skipped, Info };
    std::string name;
    State state = State::Ok;
    std::string detail;
    // What to do about a failure, one sentence per entry.
    std::vector<std::string> advice;
};

struct CheckApiReport {
    std::vector<CheckStep> steps;
    // The machine-readable summary: key=value pairs in this order (values without blanks).
    std::vector<std::pair<std::string, std::string>> summary;

    void add(CheckStep step) { steps.push_back(std::move(step)); }
    void set(std::string key, std::string value) {
        summary.emplace_back(std::move(key), std::move(value));
    }
    // True if no step failed.
    bool passed() const;
};

// "Step 1 of 5" style lines, the advice indented under a failed step, a blank line, and the
// summary line "CHECK_API_RESULT key=value ...". The text is plain ASCII.
std::string formatCheckApiReport(const CheckApiReport& report);
std::string checkApiSummaryLine(const CheckApiReport& report);

// What to tell the owner when a request failed. `auth` is the sign-in header that was used (the
// advice for a rejected key suggests the other one). `effort` and `thinking` are the settings the
// request carried: a refused request (HTTP 400) that carried one gets a line saying that the
// model may not support it.
std::vector<std::string>
adviceForError(const providers::ChatError& error, AuthMode auth,
               providers::Effort effort = providers::Effort::Default,
               providers::Thinking thinking = providers::Thinking::Default);

// "--effort low --thinking disabled" for the settings that are not Default; empty if none is.
std::string settingsFlags(providers::Effort effort, providers::Thinking thinking);

// The same for a failure before a request could be made: the key or the model is missing.
std::vector<std::string> adviceForMissing(const std::vector<std::string>& missingVariables);

const char* authName(AuthMode auth) noexcept;

// The long, fixed text that the cache probe sends as its first system block (about 32 KB, a
// little under 10,000 tokens: above every cache minimum of the current models). Deterministic, so
// the two probe requests are byte-identical; plain ASCII, harmless filler that mentions no game.
std::string cacheProbeText(std::size_t bytes = 32 * 1024);

// The outcome of two identical requests with a long shared start.
struct CacheProbeVerdict {
    // "hit", "written_not_read" or "miss".
    std::string result;
    // Tokens the first request had in total (input + cache read + cache write).
    int prefixTokens = 0;
    // What to print (one line each).
    std::vector<std::string> lines;
};

CacheProbeVerdict judgeCacheProbe(const providers::Usage& first, const providers::Usage& second);

}  // namespace curia
