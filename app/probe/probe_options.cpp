// SPDX-License-Identifier: MIT
#include "probe_options.h"

#include <cerrno>
#include <cstdlib>

#include "probe_combos.h"

namespace curia::probe {

namespace {

bool parseDouble(const std::string& text, double* out) {
    if (text.empty()) {
        return false;
    }
    char* end = nullptr;
    errno = 0;
    const double value = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == nullptr || *end != '\0') {
        return false;
    }
    *out = value;
    return true;
}

bool parseStage(const std::string& text, Stage* out) {
    struct Named {
        const char* name;
        Stage stage;
    };
    static const Named kNames[] = {
        {"e1", Stage::E1},
        {"e2", Stage::E2},
        {"e3", Stage::E3},
        {"e4", Stage::E4},
        {"interactive", Stage::Interactive},
        {"detect", Stage::Detect},
        {"tray", Stage::Tray},
    };
    for (const auto& named : kNames) {
        if (text == named.name) {
            *out = named.stage;
            return true;
        }
    }
    return false;
}

}  // namespace

const char* stageName(Stage stage) {
    switch (stage) {
    case Stage::E1:
        return "e1";
    case Stage::E2:
        return "e2";
    case Stage::E3:
        return "e3";
    case Stage::E4:
        return "e4";
    case Stage::Interactive:
        return "interactive";
    case Stage::Detect:
        return "detect";
    case Stage::Tray:
        return "tray";
    }
    return "unknown";
}

ParseResult parseArgs(const std::vector<std::string>& args) {
    ParseResult result;
    bool haveStage = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        std::string key = args[i];
        if (key.rfind("-psn_", 0) == 0) {
            continue;  // old LaunchServices process serial number argument
        }
        std::string value;
        bool haveValue = false;
        const auto eq = key.find('=');
        if (key.rfind("--", 0) == 0 && eq != std::string::npos) {
            value = key.substr(eq + 1);
            key = key.substr(0, eq);
            haveValue = true;
        }

        if (key == "--no-sound") {
            result.options.sound = false;
            continue;
        }
        if (key == "--echo") {
            result.options.echo = true;
            continue;
        }

        const bool takesValue = key == "--stage" || key == "--combo" || key == "--step-seconds" ||
                                key == "--seconds" || key == "--max-seconds" ||
                                key == "--tray-delay" || key == "--results-dir" ||
                                key == "--start-delay" || key == "--display";
        if (!takesValue) {
            result.error = "unknown argument: " + args[i];
            return result;
        }
        if (!haveValue) {
            if (i + 1 >= args.size()) {
                result.error = "missing value for " + key;
                return result;
            }
            value = args[++i];
        }

        double number = 0.0;
        if (key == "--stage") {
            if (!parseStage(value, &result.options.stage)) {
                result.error = "unknown stage: " + value + " (e1 e2 e3 e4 interactive detect tray)";
                return result;
            }
            haveStage = true;
        } else if (key == "--results-dir") {
            if (value.empty()) {
                result.error = "--results-dir needs a directory";
                return result;
            }
            result.options.resultsDir = value;
        } else if (!parseDouble(value, &number)) {
            result.error = "not a number for " + key + ": " + value;
            return result;
        } else if (key == "--combo") {
            const auto count = static_cast<int>(allCombos().size());
            if (number != static_cast<int>(number) || number < 1 || number > count) {
                result.error = "--combo must be a whole number from 1 to " + std::to_string(count);
                return result;
            }
            result.options.combo = static_cast<int>(number);
        } else if (key == "--step-seconds") {
            if (number <= 0.0) {
                result.error = "--step-seconds must be positive";
                return result;
            }
            result.options.stepSeconds = number;
        } else if (key == "--seconds") {
            if (number <= 0.0) {
                result.error = "--seconds must be positive";
                return result;
            }
            result.options.seconds = number;
        } else if (key == "--max-seconds") {
            if (number <= 0.0) {
                result.error = "--max-seconds must be positive";
                return result;
            }
            result.options.maxSeconds = number;
        } else if (key == "--start-delay") {
            if (number < 0.0) {
                result.error = "--start-delay must not be negative";
                return result;
            }
            result.options.startDelaySeconds = number;
        } else if (key == "--display") {
            if (number != static_cast<int>(number) || number < 0) {
                result.error = "--display must be a whole number from 0 (the primary display)";
                return result;
            }
            result.options.displayIndex = static_cast<int>(number);
        } else {  // --tray-delay
            if (number < 0.0) {
                result.error = "--tray-delay must not be negative";
                return result;
            }
            result.options.trayDelaySeconds = number;
        }
    }

    if (!haveStage) {
        result.error = "missing --stage (e1 e2 e3 e4 interactive detect tray)";
        return result;
    }
    result.ok = true;
    return result;
}

double runSeconds(const ProbeOptions& options) {
    const auto pick = [&](double fallback) {
        return options.seconds > 0.0 ? options.seconds : fallback;
    };
    switch (options.stage) {
    case Stage::E1:
    case Stage::E2:
        return pick(20.0);
    case Stage::E3:
        if (options.combo == 0) {
            return options.stepSeconds * static_cast<double>(allCombos().size() + 1U);
        }
        return pick(60.0);
    case Stage::E4:
    case Stage::Detect:
        return pick(60.0);
    case Stage::Interactive:
    case Stage::Tray:
        return options.seconds > 0.0 ? options.seconds : 0.0;
    }
    return 0.0;
}

}  // namespace curia::probe
