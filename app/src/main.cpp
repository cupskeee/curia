// SPDX-License-Identifier: MIT
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

#include "app.h"
#include "core/build_info.h"
#include "core/cli.h"
#include "core/headless_chat.h"
#include "core/watch_log.h"
#include "headless/headless_modes.h"
#include "ui/hotkey.h"

int main(int argc, char** argv) {
    curia::CliOptions options;
    std::string error;
    if (!curia::parseCli(argc, argv, options, error)) {
        std::fprintf(stderr, "curia: %s\n%s", error.c_str(),
                     std::string(curia::cliUsage()).c_str());
        return 2;
    }
    if (options.showHelp) {
        std::printf("%s", std::string(curia::cliUsage()).c_str());
        return 0;
    }
    if (options.showVersion) {
        std::printf("%s\n", curia::buildInfoJson().c_str());
        return 0;
    }
    if (options.selftestHotkey) {
        return curia::ui::runHotKeySelfTest();  // no window (macOS; the parser refuses it
                                                // elsewhere)
    }
    // Headless: must run before runApp, which initialises SDL and opens a window.
    if (!options.watchLogPath.empty()) {
        return curia::runWatchLog(options, std::cout, std::cerr);
    }
    if (options.headlessChat) {
        curia::HeadlessChatResult result;
        try {
            result = curia::headless::runHeadlessChatMode(options, std::cin, std::cout, std::cerr);
        } catch (...) {
            // No text of the exception is printed here: it could hold anything.
            std::fputs("curia: internal error\n", stderr);
            return curia::kHeadlessExitInternal;
        }
        std::cout << std::flush;
        std::cerr << std::flush;
        if (result.inputStillBlocked) {
            // A read of the terminal is still pending in the reader thread; leave without
            // waiting for it.
            std::_Exit(result.exitCode);
        }
        return result.exitCode;
    }
    if (options.checkApi) {
        try {
            return curia::headless::runCheckApiMode(options, std::cout, std::cerr);
        } catch (...) {
            std::fputs("curia: internal error\n", stderr);
            return curia::kHeadlessExitInternal;
        }
    }
    try {
        return curia::runApp(options);
    } catch (...) {
        // As in the headless modes, no text of the exception is printed: it could hold anything
        // (the window holds the key and the player's words). The window's parts were destroyed on
        // the way here in their usual order (the watcher stops, the request is cancelled, the
        // provider joins its net thread).
        std::fputs("curia: internal error\n", stderr);
        return 1;
    }
}
