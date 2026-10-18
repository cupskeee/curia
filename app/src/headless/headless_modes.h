// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>
#include <memory>
#include <string>

#include "core/app_log.h"
#include "core/cli.h"
#include "core/dev_config.h"
#include "core/headless_chat.h"
#include "http/anthropic_provider.h"
#include "providers/provider.h"

// The two command-line modes that need the network layer (curia_http): they build the Anthropic
// provider from the development configuration and run the shared logic of curia_core. No SDL, no
// window. Linked into the `curia` executable (the full-app build only).
namespace curia::headless {

// `curia --headless-chat` (core/headless_chat.h has the events and the exit codes).
HeadlessChatResult runHeadlessChatMode(const CliOptions& options, std::istream& in,
                                       std::ostream& out, std::ostream& err);

// `curia --check-api`: the owner's preflight against the real service (or, in tests, a mock
// server). Plain lines on `out` and a final machine-readable line "CHECK_API_RESULT ...". Exit 0
// all steps passed, 1 a step failed, 2 not configured or a bad endpoint, 3 timeout.
int runCheckApiMode(const CliOptions& options, std::ostream& out, std::ostream& err);

// Helpers shared by the two modes and the chat window.
// The app's own log: --log-dir, else the platform default folder (a no-op logger if neither
// works).
std::unique_ptr<AppLog> openAppLog(const CliOptions& options);
http::AuthStyle toAuthStyle(AuthMode mode);

// The Anthropic provider of the development configuration: the key and the model from `config`
// (the key is moved out of it), the endpoint from --test-endpoint (development builds only, the
// rules of net/endpoint_policy.h) or the real service, the header from --auth.
struct ProviderSetup {
    // Null when the configuration is incomplete (describeNotConfigured() says why) or the endpoint
    // was refused.
    std::shared_ptr<providers::Provider> provider;
    std::string model;
    // Why the endpoint was refused (a usage error; nothing was sent). Empty otherwise.
    std::string error;
};
ProviderSetup makeProvider(const CliOptions& options, DevConfig& config, AppLog* log);

}  // namespace curia::headless
