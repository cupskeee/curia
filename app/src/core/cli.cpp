// SPDX-License-Identifier: MIT
#include "core/cli.h"

#include <charconv>
#include <string_view>

namespace curia {

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
            const std::string_view value = argv[++i];
            int seconds = 0;
            const auto result = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
                seconds < 1 || seconds > 3600) {
                error = "--idle-test needs a whole number of seconds between 1 and 3600";
                return false;
            }
            out.idleSeconds = seconds;
        } else {
            error = "unknown argument: " + std::string(arg);
            return false;
        }
    }
    if (out.smoke && out.idleSeconds > 0) {
        error = "--smoke and --idle-test cannot be combined";
        return false;
    }
    return true;
}

std::string_view cliUsage() {
    return "Usage: curia [--smoke | --idle-test <seconds>] [--version] [--help]\n"
           "  --smoke              open a window, render 3 frames, exit (CI smoke test)\n"
           "  --idle-test <sec>    run the event loop and check that nothing renders while idle\n"
           "  --version            print build information as JSON\n";
}

}  // namespace curia
