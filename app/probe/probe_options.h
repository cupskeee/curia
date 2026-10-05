// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

namespace curia::probe {

// One stage per experiment group of docs/spikes/m2-checklist.md.
enum class Stage {
    E1,           // control: plain SDL window, Regular activation policy
    E2,           // plain SDL window, Accessory policy
    E3,           // own non-activating panel, level and flag combinations (cycles by itself)
    E4,           // transparent panel, CPU time log
    Interactive,  // E5-E8: clicks, keys, dismiss, hotkeys, click-through
    Detect,       // E9: frontmost application, CK3 window rectangle, permission pre-checks
    Tray,         // E11: menu-bar item
};

const char* stageName(Stage stage);

struct ProbeOptions {
    Stage stage = Stage::E3;
    int combo = 0;                    // 0 = stage default (E3 cycles; the others use combo 1)
    double stepSeconds = 5.0;         // E3: seconds per combination
    double seconds = 0.0;             // 0 = stage default, see runSeconds()
    double maxSeconds = 900.0;        // hard cap for every stage
    double trayDelaySeconds = 0.0;    // Tray: create the menu-bar item this many seconds after "go"
    double startDelaySeconds = 15.0;  // time to bring CK3 to the front; the stage starts at "go"
    int displayIndex = -1;  // NSScreen index for the panel; -1 = the display under the pointer
    bool sound = true;
    bool echo = false;       // also print log lines to stderr
    std::string resultsDir;  // empty = ~/curia_m2_results
};

struct ParseResult {
    bool ok = false;
    ProbeOptions options;
    std::string error;
};

// `args` excludes argv[0]. Accepts "--key value" and "--key=value"; ignores macOS "-psn_"
// arguments.
ParseResult parseArgs(const std::vector<std::string>& args);

// Planned run length in seconds counted from "go"; 0 means "until the owner quits" (the hard cap,
// counted from launch, still applies). E3 cycling shows each combination once and then replays
// combination 1 as a last step, to expose order dependence.
double runSeconds(const ProbeOptions& options);

}  // namespace curia::probe
