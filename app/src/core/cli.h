// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace curia {

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
};

// Parses command-line arguments. Returns false and sets `error` on invalid input.
bool parseCli(int argc, const char* const* argv, CliOptions& out, std::string& error);

std::string_view cliUsage();

}  // namespace curia
