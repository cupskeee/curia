// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "providers/chat_types.h"

namespace curia {

// Which header carries the API key (research-notes C11). The HTTP layer maps it to its own enum.
enum class AuthMode { Bearer, XApiKey };

struct CliOptions {
    bool showHelp = false;
    bool showVersion = false;
    // Open a window, render three frames with the software renderer, print CURIA_SMOKE_OK, exit.
    bool smoke = false;
    // Run the normal event loop for N seconds and verify that no frames are drawn while idle.
    int idleSeconds = 0;
    // Headless mode (no SDL, no window): tail this log file and print one JSON object per event.
    // Empty = not in this mode.
    std::string watchLogPath;
    // --watch-log: read the file from its first byte instead of attaching at its end.
    bool fromStart = false;
    // --watch-log: exit 0 after this many complete frames; 0 = not given.
    int maxFrames = 0;
    // --watch-log: stop after this many seconds; 0 = not given (runWatchLog applies its default).
    int timeoutSeconds = 0;

    // Headless chat (no SDL, no window): read the game's log like the app does, take the newest
    // complete frame (or --frame; both need --from-start: without it the log is tailed from its
    // end and the run has no snapshot until the game writes one), and answer questions typed as
    // commands on stdin. One JSON
    // object per line on stdout. See core/headless_chat.h.
    bool headlessChat = false;
    // --log: the game's debug.log to read (headless chat).
    std::string logPath;
    // --frame: use the snapshot with this number instead of the newest one (needs --from-start).
    std::optional<int> frameSnap;
    // --max-tokens: the answer limit; 0 = the default (4096).
    int maxTokens = 0;
    // --show-thinking: ask the provider to include the model's reasoning text (shown as a
    // separate event kind; the chat view never shows it).
    bool showThinking = false;

    // The owner's preflight (needs the network): key, model, TLS, sign-in header, a tiny
    // completion and, with --cache-probe, whether the prompt cache works.
    bool checkApi = false;
    bool cacheProbe = false;

    // Shared by headless chat and --check-api.
    // --effort low|medium|high|xhigh|max and --thinking adaptive|between_tools|disabled: sent as
    // the request's output_config.effort and thinking.type; not given = not sent (Default). No
    // secret in them, so they work in every build. A model that does not support a value answers
    // HTTP 400.
    providers::Effort effort = providers::Effort::Default;
    providers::Thinking thinking = providers::Thinking::Default;
    // --test-endpoint http://127.0.0.1:PORT: a mock server (development builds only; the rules
    // are net/endpoint_policy.h). Empty = the real service.
    std::string testEndpoint;
    AuthMode auth = AuthMode::Bearer;
    // --log-dir: the folder of the app's own log (curia.log); empty = the platform default.
    std::string appLogDir;

    // Test-only timing, meaningful only in a build with CURIA_ENABLE_DEV_ENV (the headless modes
    // refuse them otherwise): thresholds of the log-silence detectors and the polling intervals,
    // in milliseconds, so that the end-to-end tests do not wait for the real ones.
    std::optional<int> armedWaitMs;
    std::optional<int> armSnapshotAgeMs;
    std::optional<int> passiveSilenceMs;
    std::optional<int> tickMs;
    std::optional<int> pollMs;
    // Throws an exception inside the headless chat after the first answer text, to prove that the
    // fatal-error path prints nothing secret (same build rule).
    bool selftestThrow = false;

    // True if any of the test-only options was given.
    bool hasTestOnlyOptions() const {
        return armedWaitMs.has_value() || armSnapshotAgeMs.has_value() ||
               passiveSilenceMs.has_value() || tickMs.has_value() || pollMs.has_value() ||
               selftestThrow;
    }
};

// Parses command-line arguments. Returns false and sets `error` on invalid input.
bool parseCli(int argc, const char* const* argv, CliOptions& out, std::string& error);

std::string_view cliUsage();

}  // namespace curia
