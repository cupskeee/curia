// SPDX-License-Identifier: MIT
#include "core/cli.h"

#include <charconv>
#include <string_view>

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
        } else {
            error = "unknown argument: " + std::string(arg);
            return false;
        }
    }
    if (out.smoke && out.idleSeconds > 0) {
        error = "--smoke and --idle-test cannot be combined";
        return false;
    }
    if (!out.watchLogPath.empty() && (out.smoke || out.idleSeconds > 0)) {
        error = "--watch-log cannot be combined with --smoke or --idle-test";
        return false;
    }
    if (out.watchLogPath.empty() &&
        (out.fromStart || out.maxFrames > 0 || out.timeoutSeconds > 0)) {
        error = "--from-start, --max-frames and --timeout-seconds need --watch-log";
        return false;
    }
    return true;
}

std::string_view cliUsage() {
    return "Usage: curia [--smoke | --idle-test <seconds> | --watch-log <path> [--from-start]\n"
           "             [--max-frames <n>] [--timeout-seconds <s>]] [--version] [--help]\n"
           "  --smoke              open a window, render 3 frames, exit (CI smoke test)\n"
           "  --idle-test <sec>    run the event loop and check that nothing renders while idle\n"
           "  --watch-log <path>   no window: tail a log file and print one JSON object per\n"
           "                       bridge event (JSON Lines). Attaches at the end of an existing\n"
           "                       file unless --from-start. Exit 0 after --max-frames complete\n"
           "                       frames, or at the timeout when no frame count was asked for\n"
           "                       (default 60 s); exit 3 on a timeout before the frames\n"
           "  --version            print build information as JSON\n";
}

}  // namespace curia
