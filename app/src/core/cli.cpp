// SPDX-License-Identifier: MIT
#include "core/cli.h"

#include <charconv>
#include <optional>
#include <string_view>

#include "core/redact.h"

namespace curia {

namespace {

bool parseWholeNumber(std::string_view text, int min, int max, int& out) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size() || value < min ||
        value > max) {
        return false;
    }
    out = value;
    return true;
}

constexpr int kMinWindowSide = 200;
constexpr int kMaxWindowSide = 8000;

// "520x680" (also with a capital X).
bool parseWindowSize(std::string_view text, int& width, int& height) {
    const std::size_t x = text.find_first_of("xX");
    if (x == std::string_view::npos) {
        return false;
    }
    int w = 0;
    int h = 0;
    if (!parseWholeNumber(text.substr(0, x), kMinWindowSide, kMaxWindowSide, w) ||
        !parseWholeNumber(text.substr(x + 1), kMinWindowSide, kMaxWindowSide, h)) {
        return false;
    }
    width = w;
    height = h;
    return true;
}

// An argument that is safe to repeat in an error message: a flag-shaped word of reasonable
// length, with anything key-shaped scrubbed. A stray positional word is never repeated (a key
// pasted by mistake must not be echoed back).
std::string describeArgument(std::string_view arg) {
    if (arg.size() > 2 && arg.substr(0, 2) == "--" && arg.size() <= 40) {
        return redact(arg);
    }
    return "(not a recognised option)";
}

// Reads the value of a flag that takes a number. Returns false (and sets `error`) if it is
// missing or out of range.
bool numberFlag(int argc, const char* const* argv, int& i, std::string_view flag, int min, int max,
                int& out, std::string& error) {
    if (i + 1 >= argc || !parseWholeNumber(argv[i + 1], min, max, out)) {
        error = std::string(flag) + " needs a whole number between " + std::to_string(min) +
                " and " + std::to_string(max);
        return false;
    }
    ++i;
    return true;
}

bool optionalNumberFlag(int argc, const char* const* argv, int& i, std::string_view flag, int min,
                        int max, std::optional<int>& out, std::string& error) {
    int value = 0;
    if (!numberFlag(argc, argv, i, flag, min, max, value, error)) {
        return false;
    }
    out = value;
    return true;
}

bool textFlag(int argc, const char* const* argv, int& i, std::string_view flag, std::string& out,
              std::string& error) {
    if (i + 1 >= argc || std::string_view(argv[i + 1]).empty()) {
        error = std::string(flag) + " needs a value";
        return false;
    }
    out = argv[++i];
    return true;
}

int modeCount(const CliOptions& o) {
    int count = 0;
    count += o.smoke ? 1 : 0;
    count += o.idleSeconds > 0 ? 1 : 0;
    count += !o.watchLogPath.empty() ? 1 : 0;
    count += o.headlessChat ? 1 : 0;
    count += o.checkApi ? 1 : 0;
    return count;
}

bool validate(const CliOptions& out, std::string& error) {
    if (out.smoke && out.idleSeconds > 0) {
        error = "--smoke and --idle-test cannot be combined";
        return false;
    }
    if (modeCount(out) > 1) {
        error = "only one of --smoke, --idle-test, --watch-log, --headless-chat and --check-api "
                "can be used at a time";
        return false;
    }
    const bool watch = !out.watchLogPath.empty();
    if (!watch && out.maxFrames > 0) {
        error = "--max-frames needs --watch-log";
        return false;
    }
    if (!watch && !out.headlessChat && (out.fromStart || out.timeoutSeconds > 0)) {
        error = "--from-start and --timeout-seconds need --watch-log or --headless-chat";
        return false;
    }
    if (out.headlessChat && out.logPath.empty()) {
        error = "--headless-chat needs --log <path of the game's debug.log>";
        return false;
    }
    const bool window = out.windowMode();
    if (!out.headlessChat && !window && !out.logPath.empty()) {
        error = "--log needs --headless-chat or the chat window (no mode flag)";
        return false;
    }
    const bool headlessOnly = out.frameSnap.has_value() || out.maxTokens > 0 || out.showThinking ||
                              out.hasTestOnlyOptions();
    if (!out.headlessChat && headlessOnly) {
        error = "--frame, --max-tokens, --show-thinking and the test-only timing options need "
                "--headless-chat";
        return false;
    }
    if (!out.smoke && (!out.screenshotPath.empty() || out.windowWidth > 0)) {
        error = "--screenshot and --window-size need --smoke";
        return false;
    }
    if (out.frameSnap.has_value() && !out.fromStart) {
        error = "--frame needs --from-start (without it the log is read from its end, so no "
                "earlier snapshot is ever seen)";
        return false;
    }
    if (out.cacheProbe && !out.checkApi) {
        error = "--cache-probe needs --check-api";
        return false;
    }
    if (!out.headlessChat && !out.checkApi && (out.effortGiven || out.thinkingGiven)) {
        error = "--effort and --thinking need --headless-chat or --check-api";
        return false;
    }
    if (!out.headlessChat && !out.checkApi && !window &&
        (!out.testEndpoint.empty() || out.auth != AuthMode::Bearer || !out.appLogDir.empty())) {
        error = "--test-endpoint, --auth and --log-dir need --headless-chat, --check-api or the "
                "chat window (no mode flag)";
        return false;
    }
    return true;
}

}  // namespace

bool parseCli(int argc, const char* const* argv, CliOptions& out, std::string& error) {
    out = CliOptions{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            out.showHelp = true;
        } else if (arg == "--version") {
            out.showVersion = true;
        } else if (arg == "--smoke") {
            out.smoke = true;
        } else if (arg == "--screenshot") {
            if (!textFlag(argc, argv, i, arg, out.screenshotPath, error)) {
                return false;
            }
        } else if (arg == "--window-size") {
            if (i + 1 >= argc || !parseWindowSize(argv[i + 1], out.windowWidth, out.windowHeight)) {
                error = "--window-size needs WIDTHxHEIGHT, each a whole number between " +
                        std::to_string(kMinWindowSide) + " and " + std::to_string(kMaxWindowSide);
                return false;
            }
            ++i;
        } else if (arg == "--idle-test") {
            if (i + 1 >= argc) {
                error = "--idle-test needs a number of seconds";
                return false;
            }
            if (!parseWholeNumber(argv[++i], 1, 3600, out.idleSeconds)) {
                error = "--idle-test needs a whole number of seconds between 1 and 3600";
                return false;
            }
        } else if (arg == "--watch-log") {
            if (i + 1 >= argc || std::string_view(argv[i + 1]).empty()) {
                error = "--watch-log needs the path of a log file";
                return false;
            }
            out.watchLogPath = argv[++i];
        } else if (arg == "--from-start") {
            out.fromStart = true;
        } else if (arg == "--max-frames") {
            if (i + 1 >= argc || !parseWholeNumber(argv[++i], 1, 1000000, out.maxFrames)) {
                error = "--max-frames needs a whole number between 1 and 1000000";
                return false;
            }
        } else if (arg == "--timeout-seconds") {
            if (i + 1 >= argc || !parseWholeNumber(argv[++i], 1, 86400, out.timeoutSeconds)) {
                error = "--timeout-seconds needs a whole number of seconds between 1 and 86400";
                return false;
            }
        } else if (arg == "--headless-chat") {
            out.headlessChat = true;
        } else if (arg == "--log") {
            if (!textFlag(argc, argv, i, arg, out.logPath, error)) {
                return false;
            }
        } else if (arg == "--frame") {
            if (!optionalNumberFlag(argc, argv, i, arg, 1, 2147483647, out.frameSnap, error)) {
                return false;
            }
        } else if (arg == "--max-tokens") {
            if (!numberFlag(argc, argv, i, arg, 1, 64000, out.maxTokens, error)) {
                return false;
            }
        } else if (arg == "--show-thinking") {
            out.showThinking = true;
        } else if (arg == "--check-api") {
            out.checkApi = true;
        } else if (arg == "--cache-probe") {
            out.cacheProbe = true;
        } else if (arg == "--test-endpoint") {
            if (!textFlag(argc, argv, i, arg, out.testEndpoint, error)) {
                return false;
            }
        } else if (arg == "--auth") {
            if (i + 1 >= argc) {
                error = "--auth needs bearer or x-api-key";
                return false;
            }
            const std::string_view value = argv[++i];
            if (value == "bearer") {
                out.auth = AuthMode::Bearer;
            } else if (value == "x-api-key") {
                out.auth = AuthMode::XApiKey;
            } else {
                error = "--auth needs bearer or x-api-key";
                return false;
            }
        } else if (arg == "--effort") {
            // "default" is a word of the command line only: it sends no effort setting.
            std::optional<providers::Effort> effort;
            if (i + 1 < argc) {
                effort = std::string_view(argv[i + 1]) == "default"
                             ? std::optional<providers::Effort>(providers::Effort::Default)
                             : providers::parseEffort(argv[i + 1]);
            }
            if (!effort.has_value()) {
                error = "--effort needs default, low, medium, high, xhigh or max";
                return false;
            }
            out.effort = *effort;
            out.effortGiven = true;
            ++i;
        } else if (arg == "--thinking") {
            const std::optional<providers::Thinking> thinking =
                i + 1 < argc ? providers::parseThinking(argv[i + 1]) : std::nullopt;
            if (!thinking.has_value()) {
                error = "--thinking needs adaptive, between_tools or disabled";
                return false;
            }
            out.thinking = *thinking;
            out.thinkingGiven = true;
            ++i;
        } else if (arg == "--log-dir") {
            if (!textFlag(argc, argv, i, arg, out.appLogDir, error)) {
                return false;
            }
        } else if (arg == "--armed-wait-ms") {
            if (!optionalNumberFlag(argc, argv, i, arg, 1, 3600000, out.armedWaitMs, error)) {
                return false;
            }
        } else if (arg == "--arm-snapshot-age-ms") {
            if (!optionalNumberFlag(argc, argv, i, arg, 0, 3600000, out.armSnapshotAgeMs, error)) {
                return false;
            }
        } else if (arg == "--passive-silence-ms") {
            if (!optionalNumberFlag(argc, argv, i, arg, 1, 3600000, out.passiveSilenceMs, error)) {
                return false;
            }
        } else if (arg == "--tick-ms") {
            if (!optionalNumberFlag(argc, argv, i, arg, 1, 60000, out.tickMs, error)) {
                return false;
            }
        } else if (arg == "--poll-ms") {
            if (!optionalNumberFlag(argc, argv, i, arg, 1, 60000, out.pollMs, error)) {
                return false;
            }
        } else if (arg == "--selftest-throw") {
            out.selftestThrow = true;
        } else {
            error = "unknown argument: " + describeArgument(arg);
            return false;
        }
    }
    return validate(out, error);
}

// The usage text names the default effort.
static_assert(providers::kDefaultEffort == providers::Effort::Low,
              "update the --effort line of cliUsage()");

std::string_view cliUsage() {
    return "Usage: curia [--log <path>] [--test-endpoint <url>] [--auth bearer|x-api-key]\n"
           "             [--log-dir <dir>]\n"
           "       curia [--smoke [--screenshot <file.bmp>] [--window-size <W>x<H>] |\n"
           "             --idle-test <seconds> | --watch-log <path> [--from-start]\n"
           "             [--max-frames <n>] [--timeout-seconds <s>]] [--version] [--help]\n"
           "       curia --headless-chat --log <path> [--from-start] [--frame <snap>]\n"
           "             [--test-endpoint <url>] [--auth bearer|x-api-key] [--max-tokens <n>]\n"
           "             [--effort <level>] [--thinking <mode>] [--show-thinking]\n"
           "             [--log-dir <dir>] [--timeout-seconds <s>]\n"
           "       curia --check-api [--cache-probe] [--auth bearer|x-api-key]\n"
           "             [--effort <level>] [--thinking <mode>] [--test-endpoint <url>]\n"
           "             [--log-dir <dir>]\n"
           "  (no mode flag)       the chat window. It reads CK3's debug.log from CK3's own\n"
           "                       folder (or --log <path>), from its end: the realm appears once\n"
           "                       the player clicks Advisor in the game. The API key and the\n"
           "                       model come from the environment (development builds)\n"
           "  --smoke              open a window, draw a canned conversation with a scripted\n"
           "                       answer (no network, no key, no game), exit (CI smoke test);\n"
           "                       --screenshot saves the last frame, --window-size sets the\n"
           "                       window's size in logical pixels\n"
           "  --idle-test <sec>    run the event loop and check that nothing renders while idle\n"
           "  --watch-log <path>   no window: tail a log file and print one JSON object per\n"
           "                       bridge event (JSON Lines). Attaches at the end of an existing\n"
           "                       file unless --from-start. Exit 0 after --max-frames complete\n"
           "                       frames, or at the timeout when no frame count was asked for\n"
           "                       (default 60 s); exit 3 on a timeout before the frames\n"
           "  --headless-chat      no window: read the game's log like the app, answer questions\n"
           "                       typed as commands on stdin (ask <text>, cancel, arm, refresh,\n"
           "                       retry, ck3 running|stopped, quit; end of input quits once the\n"
           "                       work is done), print one JSON object per event. The API key "
           "and\n"
           "                       the model come from the environment (development builds).\n"
           "                       --from-start reads the log from its beginning and uses its\n"
           "                       newest complete snapshot (--frame <n> picks snapshot n);\n"
           "                       without it the run attaches at the end of the log and has no\n"
           "                       snapshot until the game writes one\n"
           "                       Exit 0 ok, 1 an answer failed, 2 bad arguments or not\n"
           "                       configured (nothing sent), 3 timeout, 4 internal error\n"
           "  --check-api          print the result of each step of a connection check against\n"
           "                       the real service (key, model, TLS, sign-in header, one tiny\n"
           "                       answer; --cache-probe also tests the prompt cache). Exit 0 ok,\n"
           "                       1 a step failed, 2 not configured, 3 timeout\n"
           "  --effort <level>     with --headless-chat or --check-api: the effort setting low,\n"
           "                       medium, high, xhigh or max. Without it Curia sends low;\n"
           "                       --effort default sends none (the model's default applies)\n"
           "  --thinking <mode>    likewise the thinking setting: adaptive, between_tools or\n"
           "                       disabled (not given: none is sent). A model that does not\n"
           "                       support a value answers HTTP 400\n"
           "  --version            print build information as JSON\n";
}

}  // namespace curia
