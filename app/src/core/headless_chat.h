// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/app_log.h"
#include "core/cli.h"
#include "core/redact.h"
#include "platform/env.h"
#include "providers/provider.h"

namespace curia {

// `curia --headless-chat`: the advisor without a window. It reads the game's log with the same
// LogWatcher, keeps the snapshot with the same AppController and talks to the provider through the
// same ChatSession as the app does; stdin carries the player's actions (headless_commands.h) and
// stdout the events, one JSON object per line (json_lines.h).
//
// Exit codes
//   0  the session ended normally (every question that was asked got its answer, or was cancelled
//      on purpose)
//   1  at least one question ended in an error
//   2  bad arguments or setup, nothing was sent: no key or model ("not_configured"), a log file
//      that does not exist (or is a folder), a snapshot number that is not in the log, a
//      transcript folder inside a source tree or one that cannot be written to, test-only options
//      in a build that does not accept them
//   3  the overall time limit (--timeout-seconds, default 30 minutes) ran out
//   4  an internal error (an exception): a "fatal" event with the redacted text is printed
inline constexpr int kHeadlessExitOk = 0;
inline constexpr int kHeadlessExitFailed = 1;
inline constexpr int kHeadlessExitSetup = 2;
inline constexpr int kHeadlessExitTimeout = 3;
inline constexpr int kHeadlessExitInternal = 4;

inline constexpr int kDefaultHeadlessTimeoutSeconds = 1800;
inline constexpr int kDefaultHeadlessTickMs = 500;

// Events (all on stdout, key "event"; the text values are redacted):
//   ready          the log is read up to its end at the start (or attached, when it is tailed
//                  from its end): {from_start, snapshot (snap or null), date, frames, model,
//                  effort, thinking, persona_version, transcripts (folder or null)}; effort and
//                  thinking are the wire words the requests carry, "default" for none sent: the
//                  effort is Curia's default ("low", D22) unless --effort is given ("--effort
//                  default" sends none), the thinking "default" unless --thinking is given
//   snapshot       a frame was stored or the export became unreadable: {sequence, snap, date,
//                  complete, unreadable, probe}
//   overlay_open   the game's Advisor button was clicked (source game_click), or `arm` / `refresh`
//                  (source hotkey / refresh): {source, probe}
//   ask            a question was accepted: {generation, text}
//   status         {state: waiting | streaming | thinking}; "thinking" at most once per answer,
//                  when the model starts a thinking block before the first text
//   token          the next piece of the answer text: {text}
//   text_reset     rare: redaction changed text that was already sent; the whole text: {text}
//   done           the answer is complete: {generation, text, first_token_ms, total_ms, usage:
//                  {input, output, cache_read, cache_write}, stop_reason, max_tokens (the limit
//                  cut it off), effort, thinking (as in ready), thinking_started_ms (from the ask
//                  to the first thinking block; absent when none came before the text)}
//   cancelled      the answer was stopped: {generation, text (what was received), total_ms}
//   error          the question failed: {generation, code, message, detail, retryable, http_status,
//                  retry_after, request_id, partial_len, partial_text, total_ms}
//   notice         the log-silence notice changed: {kind: restart_ck3 | silent_soft |
//                  ck3_not_running | none}
//   ck3            the answer to the `ck3` command: {running}
//   transcript     a transcript file was written (CURIA_LOG_CONTENT): {path}
//   transcript_error  it could not be: {message}
//   ask_refused    the ask or retry was not started: {outcome}
//   cancel_ignored `cancel` with nothing in flight
//   bad_command    a stdin line was not a command: {message}
//   unsupported_format  the mod's frames are in a layout this app does not read
//   not_configured the key or the model is missing: {missing: [variable names]} (never values)
//   fatal          the run ends with an exit code other than 0 or 1: {code, message}
//
// Terminal events of one question: exactly one of done, cancelled and error.
// Order of commands: as read, except that an `ask` or `retry` waits for the question in flight to
// end without holding up the commands behind it: `cancel`, `arm`, `refresh`, `ck3` and `quit` act
// at once, even when an `ask` is queued before them ("ask a / ask b / cancel" stops a and then
// runs b; questions queued behind the one in flight keep their order).

struct HeadlessChatConfig {
    // The parsed command line (log path, frame, from-start, timing options, selftest).
    CliOptions options;
    // The model to ask (from the configuration).
    std::string model;
    // The configuration variables that are missing or unusable; non-empty = "not configured".
    std::vector<std::string> missing;
    // Notes about ignored settings (printed to stderr).
    std::vector<std::string> warnings;
    // Null with `missing` set.
    std::shared_ptr<providers::Provider> provider;
    // Write a transcript file per question (CURIA_LOG_CONTENT).
    bool logContent = false;
    std::optional<std::string> transcriptDir;
    // Looks up HOME / USERPROFILE for the default transcript folder; empty = the real environment.
    platform::GetEnv getenv;
    // Where numbers about the run go; not owned, may be null.
    AppLog* log = nullptr;
    // The secrets to scrub; null = the process-wide registry. Not owned.
    const SecretRegistry* registry = nullptr;
    // The log watcher's polling interval (the app's 250 ms by default).
    std::chrono::milliseconds pollInterval{250};
    // The input is the process's standard input: a read that is still blocked when the run ends
    // is left behind (the caller must then end the process without waiting for it).
    bool inputIsProcessStdin = false;
};

struct HeadlessChatResult {
    int exitCode = kHeadlessExitOk;
    // The reader of the input is still blocked in a read (only with inputIsProcessStdin).
    bool inputStillBlocked = false;
};

HeadlessChatResult runHeadlessChat(HeadlessChatConfig config, std::istream& in, std::ostream& out,
                                   std::ostream& err);

// True in a build that accepts the test-only options (CURIA_ENABLE_DEV_ENV).
bool testOnlyOptionsAllowed();

// "internal error: <text>" with the text scrubbed of secrets and cut to 300 bytes; never throws.
std::string describeFatalException(const char* what, const SecretRegistry* registry = nullptr);

}  // namespace curia
