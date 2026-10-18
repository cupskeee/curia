// SPDX-License-Identifier: MIT
#include "headless/headless_modes.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <utility>

#include "core/app_log.h"
#include "core/check_api_report.h"
#include "core/dev_config.h"
#include "core/json_lines.h"
#include "http/anthropic_provider.h"
#include "net/endpoint_policy.h"

namespace curia::headless {

std::unique_ptr<AppLog> openAppLog(const CliOptions& options) {
    const std::filesystem::path directory = options.appLogDir.empty()
                                                ? AppLog::defaultLogDir()
                                                : std::filesystem::path(options.appLogDir);
    return std::make_unique<AppLog>(directory);
}

http::AuthStyle toAuthStyle(AuthMode mode) {
    return mode == AuthMode::Bearer ? http::AuthStyle::Bearer : http::AuthStyle::XApiKey;
}

HeadlessChatResult runHeadlessChatMode(const CliOptions& options, std::istream& in,
                                       std::ostream& out, std::ostream& err) {
    DevConfig config = loadDevConfig();
    HeadlessChatConfig chat;
    chat.options = options;
    chat.warnings = config.warnings;
    chat.logContent = config.logContent;
    chat.transcriptDir = config.transcriptDir;
    chat.inputIsProcessStdin = true;
    if (options.pollMs.has_value()) {
        chat.pollInterval = std::chrono::milliseconds(*options.pollMs);
    }
    chat.missing = missingVariables(config);
    std::unique_ptr<AppLog> log = openAppLog(options);
    chat.log = log.get();

    if (chat.missing.empty()) {
        ProviderSetup setup = makeProvider(options, config, log.get());
        if (!setup.error.empty()) {
            JsonLineWriter writer(out);
            writer.write({{"event", "fatal"}, {"code", "setup"}, {"message", setup.error}});
            err << "curia: " << setup.error << '\n' << std::flush;
            HeadlessChatResult result;
            result.exitCode = kHeadlessExitSetup;
            return result;
        }
        chat.model = std::move(setup.model);
        chat.provider = std::move(setup.provider);
    }
    return runHeadlessChat(std::move(chat), in, out, err);
}

ProviderSetup makeProvider(const CliOptions& options, DevConfig& config, AppLog* log) {
    ProviderSetup setup;
    if (!missingVariables(config).empty() || !config.apiKey.has_value() ||
        !config.model.has_value()) {
        return setup;
    }
    net::Endpoint endpoint;
    std::optional<std::string_view> flag;
    if (!options.testEndpoint.empty()) {
        flag = std::string_view(options.testEndpoint);
    }
    if (!net::makeEndpoint(flag, &*config.apiKey, endpoint, setup.error)) {
        if (setup.error.empty()) {
            setup.error = "the endpoint was refused";
        }
        return setup;
    }
    setup.error.clear();
    http::AnthropicProviderOptions providerOptions;
    providerOptions.auth = toAuthStyle(options.auth);
    providerOptions.log = log;
    setup.model = *config.model;
    setup.provider = std::make_shared<http::AnthropicProvider>(
        std::move(endpoint), std::move(*config.apiKey), providerOptions);
    return setup;
}

}  // namespace curia::headless
