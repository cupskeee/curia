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

    if (chat.missing.empty() && config.apiKey.has_value() && config.model.has_value()) {
        net::Endpoint endpoint;
        std::string endpointError;
        std::optional<std::string_view> flag;
        if (!options.testEndpoint.empty()) {
            flag = std::string_view(options.testEndpoint);
        }
        if (!net::makeEndpoint(flag, &*config.apiKey, endpoint, endpointError)) {
            JsonLineWriter writer(out);
            writer.write({{"event", "fatal"}, {"code", "setup"}, {"message", endpointError}});
            err << "curia: " << endpointError << '\n' << std::flush;
            HeadlessChatResult result;
            result.exitCode = kHeadlessExitSetup;
            return result;
        }
        http::AnthropicProviderOptions providerOptions;
        providerOptions.auth = toAuthStyle(options.auth);
        providerOptions.log = log.get();
        chat.model = *config.model;
        chat.provider = std::make_shared<http::AnthropicProvider>(
            std::move(endpoint), std::move(*config.apiKey), providerOptions);
    }
    return runHeadlessChat(std::move(chat), in, out, err);
}

}  // namespace curia::headless
