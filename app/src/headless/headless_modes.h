// SPDX-License-Identifier: MIT
#pragma once

#include <iosfwd>
#include <memory>

#include "core/app_log.h"
#include "core/cli.h"
#include "core/headless_chat.h"
#include "http/anthropic_provider.h"

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

// Helpers shared by the two modes.
// The app's own log: --log-dir, else the platform default folder (a no-op logger if neither
// works).
std::unique_ptr<AppLog> openAppLog(const CliOptions& options);
http::AuthStyle toAuthStyle(AuthMode mode);

}  // namespace curia::headless
