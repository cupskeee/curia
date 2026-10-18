// SPDX-License-Identifier: MIT
#include "core/dev_config.h"

#include <utility>

namespace curia {

namespace {

// The only places in app/ where these names may be spelled (tools/ci/check_source_rules.sh).
constexpr const char* kKeyVariable = "CURIA_ANTHROPIC_API_KEY";
constexpr const char* kModelVariable = "CURIA_MODEL";

#if defined(CURIA_ENABLE_DEV_ENV)
constexpr const char* kLogContentVariable = "CURIA_LOG_CONTENT";
constexpr const char* kTranscriptDirVariable = "CURIA_TRANSCRIPT_DIR";
constexpr std::size_t kMaxKeyBytes = 4096;
constexpr std::size_t kMaxModelBytes = 128;

constexpr std::string_view kKeyRule =
    "CURIA_ANTHROPIC_API_KEY is not usable: it must be 8 to 4096 printable ASCII characters "
    "without spaces";
constexpr std::string_view kKeyNotRegisteredRule =
    "CURIA_ANTHROPIC_API_KEY is not usable: Curia cannot keep it out of what it writes";
constexpr std::string_view kModelRule =
    "CURIA_MODEL is not usable: it must be 1 to 128 characters from letters, digits and . _ : -";
constexpr std::string_view kLogContentWarning =
    "CURIA_LOG_CONTENT is ignored: it only counts when it is exactly 1";
constexpr std::string_view kTranscriptDirWarning =
    "CURIA_TRANSCRIPT_DIR is ignored: it must be 1 to 4096 characters without control characters";
constexpr std::size_t kMaxPathBytes = 4096;

bool validPathText(const std::string& value) {
    if (value.empty() || value.size() > kMaxPathBytes) {
        return false;
    }
    for (const char ch : value) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) {
            return false;
        }
    }
    return true;
}

bool validKey(const std::string& value) {
    if (value.size() < SecretRegistry::kMinSecretBytes || value.size() > kMaxKeyBytes) {
        return false;
    }
    for (const char ch : value) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte >= 0x7f) {
            return false;  // blank, control or non-ASCII (this also rejects a pasted newline)
        }
    }
    return true;
}

// The key as it would read inside a JSON string, or empty when that is the same text. A key made
// by hand may hold a quote or a backslash, which a request body would escape; the registry matches
// raw bytes, so the escaped form is registered too. (Real keys never need this.)
std::string jsonEscaped(const std::string& key) {
    if (key.find_first_of("\"\\") == std::string::npos) {
        return {};
    }
    std::string out;
    out.reserve(key.size() + 4);
    for (const char ch : key) {
        if (ch == '"' || ch == '\\') {
            out.push_back('\\');
        }
        out.push_back(ch);
    }
    return out;
}

bool validModel(const std::string& value) {
    if (value.empty() || value.size() > kMaxModelBytes) {
        return false;
    }
    for (const char ch : value) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' ||
                        ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

#endif  // CURIA_ENABLE_DEV_ENV

bool hasProblemAbout(const DevConfig& config, std::string_view variable) {
    for (const std::string& problem : config.problems) {
        if (problem.compare(0, variable.size(), variable) == 0) {
            return true;
        }
    }
    return false;
}

}  // namespace

DevConfig loadDevConfig(const platform::GetEnv& getenv, SecretRegistry& registry) {
    DevConfig config;
#if defined(CURIA_ENABLE_DEV_ENV)
    if (!getenv) {
        return config;
    }
    std::string key = getenv(kKeyVariable);
    if (!key.empty()) {
        if (!validKey(key)) {
            wipe(key);
            config.problems.emplace_back(kKeyRule);
        } else {
            std::string escaped = jsonEscaped(key);
            SecretString secret(std::move(key));  // wipes `key`
            if (registry.add(secret)) {
                if (!escaped.empty()) {
                    registry.add(escaped);  // best effort on top of the key itself
                }
                config.apiKey = std::move(secret);
            } else {
                config.problems.emplace_back(kKeyNotRegisteredRule);
            }
            wipe(escaped);
        }
    }
    std::string model = getenv(kModelVariable);
    if (!model.empty()) {
        if (validModel(model)) {
            config.model = std::move(model);
        } else {
            config.problems.emplace_back(kModelRule);
        }
    }
    const std::string logContent = getenv(kLogContentVariable);
    if (logContent == "1") {
        config.logContent = true;
    } else if (!logContent.empty() && logContent != "0") {
        config.warnings.emplace_back(kLogContentWarning);
    }
    std::string transcriptDir = getenv(kTranscriptDirVariable);
    if (!transcriptDir.empty()) {
        if (validPathText(transcriptDir)) {
            config.transcriptDir = std::move(transcriptDir);
        } else {
            config.warnings.emplace_back(kTranscriptDirWarning);
        }
    }
#else
    // Not even a lookup: a release build must not look at these variables.
    (void)getenv;
    (void)registry;
    config.problems.emplace_back(kDevConfigDisabledProblem);
#endif
    return config;
}

DevConfig loadDevConfig(const platform::GetEnv& getenv) {
    return loadDevConfig(getenv, SecretRegistry::instance());
}

DevConfig loadDevConfig() {
    return loadDevConfig(platform::GetEnv(platform::getEnv), SecretRegistry::instance());
}

std::string describeNotConfigured(const DevConfig& config) {
    const bool disabled = hasProblemAbout(config, kDevConfigDisabledProblem);
    if (config.apiKey.has_value() && config.model.has_value() && !disabled) {
        return {};
    }
    std::string message = "Curia is not configured.";
    for (const std::string& problem : config.problems) {
        message += " ";
        message += problem;
        message += ".";
    }
    if (!disabled) {
        if (!config.apiKey.has_value() && !hasProblemAbout(config, kKeyVariable)) {
            message += " ";
            message += kKeyVariable;
            message += " is not set.";
        }
        if (!config.model.has_value() && !hasProblemAbout(config, kModelVariable)) {
            message += " ";
            message += kModelVariable;
            message += " is not set.";
        }
        // A missing or unusable variable is named above (at least one is: both present returned
        // early).
        const bool keyNamed = !config.apiKey.has_value() || hasProblemAbout(config, kKeyVariable);
        const bool modelNamed =
            !config.model.has_value() || hasProblemAbout(config, kModelVariable);
        message += keyNamed && modelNamed ? " Set them" : " Set it";
        message += " in the environment that starts Curia, then start it again.";
    }
    return message;
}

std::string_view logContentWindowNote() {
    return "CURIA_LOG_CONTENT is set, but only --headless-chat writes transcripts; the chat "
           "window writes none";
}

std::string_view keyVariableName() {
    return kKeyVariable;
}

std::string_view modelVariableName() {
    return kModelVariable;
}

std::vector<std::string> missingVariables(const DevConfig& config) {
    std::vector<std::string> missing;
    if (!config.apiKey.has_value()) {
        missing.emplace_back(kKeyVariable);
    }
    if (!config.model.has_value()) {
        missing.emplace_back(kModelVariable);
    }
    if (hasProblemAbout(config, kDevConfigDisabledProblem) && missing.empty()) {
        missing.emplace_back(kKeyVariable);
        missing.emplace_back(kModelVariable);
    }
    return missing;
}

}  // namespace curia
