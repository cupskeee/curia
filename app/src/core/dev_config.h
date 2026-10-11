// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/redact.h"
#include "core/secret.h"
#include "platform/env.h"

namespace curia {

// Development configuration (decision D19): in M4 the API key and the model come from two
// environment variables of the process that starts Curia; M5 replaces them with the OS secure
// store and a settings screen. They are read only when the build option CURIA_ENABLE_DEV_ENV is
// on (on in development and CI, off in release builds); with it off, loadDevConfig() never asks
// the environment for anything.
//
// The variable names are string literals in dev_config.cpp and nowhere else in app/ (the source
// rules enforce that), so any message that has to name them comes from describeNotConfigured().
struct DevConfig {
    std::optional<SecretString> apiKey;
    std::optional<std::string> model;
    // What is wrong with a variable that is set but unusable, or why nothing was read. Each entry
    // names the variable and the rule, never the value.
    std::vector<std::string> problems;
};

// The problem recorded in a build without CURIA_ENABLE_DEV_ENV.
inline constexpr std::string_view kDevConfigDisabledProblem =
    "dev configuration is disabled in this build";

// Reads the two variables through `getenv` (an unset variable is an empty string).
//   * model: 1 to 128 characters of [A-Za-z0-9._:-];
//   * key: 8 to 4096 printable ASCII characters without blanks (8 because SecretRegistry refuses
//     shorter secrets, and a key that cannot be scrubbed from logs is not accepted).
// An invalid value is dropped and recorded as a problem without being repeated anywhere. A valid
// key is wiped from the temporary string it came in, wrapped in a SecretString and registered with
// the registry so that no log line can carry it.
DevConfig loadDevConfig(const platform::GetEnv& getenv, SecretRegistry& registry);
// The same with the process-wide registry (the one the app log uses).
DevConfig loadDevConfig(const platform::GetEnv& getenv);
// The same reading the process environment.
DevConfig loadDevConfig();

// The message for the chat view when the key or the model is missing or unusable: it names the
// variables and the rules, never a value. Empty if both are present.
std::string describeNotConfigured(const DevConfig& config);

}  // namespace curia
