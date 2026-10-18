// SPDX-License-Identifier: MIT
#include "core/check_api_report.h"

#include "core/dev_config.h"

namespace curia {

namespace {

const char* stateWord(CheckStep::State state) {
    switch (state) {
    case CheckStep::State::Ok:
        return "ok";
    case CheckStep::State::Failed:
        return "FAILED";
    case CheckStep::State::Skipped:
        return "skipped";
    case CheckStep::State::Info:
        return "info";
    }
    return "?";
}

std::string quoted(std::string_view name) {
    return std::string(name);
}

const char* const kFillerWords[] = {"harvest", "granary", "river",  "market", "lantern", "meadow",
                                    "bridge",  "orchard", "ledger", "harbor", "beacon",  "quarry",
                                    "village", "chapel",  "forest", "mill"};

}  // namespace

bool CheckApiReport::passed() const {
    for (const CheckStep& step : steps) {
        if (step.state == CheckStep::State::Failed) {
            return false;
        }
    }
    return true;
}

const char* authName(AuthMode auth) noexcept {
    return auth == AuthMode::Bearer ? "bearer" : "x-api-key";
}

std::string checkApiSummaryLine(const CheckApiReport& report) {
    std::string line = "CHECK_API_RESULT";
    line += " ok=";
    line += report.passed() ? "true" : "false";
    for (const auto& [key, value] : report.summary) {
        line += ' ';
        line += key;
        line += '=';
        line += value.empty() ? "-" : value;
    }
    return line;
}

std::string formatCheckApiReport(const CheckApiReport& report) {
    std::string out;
    for (const CheckStep& step : report.steps) {
        out += '[';
        out += stateWord(step.state);
        out += "] ";
        out += step.name;
        if (!step.detail.empty()) {
            out += ": ";
            out += step.detail;
        }
        out += '\n';
        for (const std::string& advice : step.advice) {
            out += "       -> ";
            out += advice;
            out += '\n';
        }
    }
    out += '\n';
    out += checkApiSummaryLine(report);
    out += '\n';
    return out;
}

std::vector<std::string> adviceForMissing(const std::vector<std::string>& missingVariables) {
    std::vector<std::string> advice;
    for (const std::string& name : missingVariables) {
        if (name == keyVariableName()) {
            advice.emplace_back(name +
                                " is missing or not usable. Set it in this Terminal window "
                                "(the sheet shows how, without echoing the key), then run the same "
                                "command again.");
        } else if (name == modelVariableName()) {
            advice.emplace_back(name +
                                " is missing or not usable. Set it to the model name you "
                                "were given (letters, digits and . _ : - only), then run the same "
                                "command again.");
        } else {
            advice.emplace_back(name + " is missing or not usable.");
        }
    }
    return advice;
}

SentSettings sentSettings(const CliOptions& options) {
    SentSettings settings;
    settings.effort = options.effort;
    settings.effortFromFlag = options.effortGiven;
    settings.thinking = options.thinking;
    return settings;
}

std::string describeSettings(const SentSettings& settings) {
    std::string words;
    if (settings.effort != providers::Effort::Default) {
        words += settings.effortFromFlag
                     ? std::string("--effort ") + providers::toString(settings.effort)
                     : std::string("effort ") + providers::toString(settings.effort) +
                           " (Curia's default)";
    }
    if (settings.thinking != providers::Thinking::Default) {
        words += std::string(words.empty() ? "" : " and ") + "--thinking " +
                 providers::toString(settings.thinking);
    }
    return words;
}

std::vector<std::string> adviceForError(const providers::ChatError& error, AuthMode auth,
                                        const SentSettings& settings) {
    using Code = providers::ChatError::Code;
    std::vector<std::string> advice;
    const std::string status =
        error.httpStatus > 0 ? "HTTP " + std::to_string(error.httpStatus) : std::string();
    switch (error.code) {
    case Code::Auth:
        advice.emplace_back(status + (status.empty() ? "" : ": ") +
                            "the service did not accept the key. Check that the key is typed "
                            "completely and has not been deleted or disabled in the console.");
        advice.emplace_back(
            std::string("The sign-in header used was ") + authName(auth) +
            ". If the key is right, run the check once more with --auth " +
            authName(auth == AuthMode::Bearer ? AuthMode::XApiKey : AuthMode::Bearer) +
            " and report which one worked.");
        break;
    case Code::BadRequest:
        if (error.httpStatus == 404) {
            advice.emplace_back(status + ": the model was not found. Check " +
                                quoted(modelVariableName()) +
                                " for a typo, or ask for the exact model name; do not guess one.");
        } else {
            advice.emplace_back(status + (status.empty() ? "" : ": ") +
                                "the service refused the request. The first thing to check is " +
                                quoted(modelVariableName()) + ".");
            const std::string carried = describeSettings(settings);
            if (!carried.empty() && error.httpStatus == 400) {
                const bool effort = settings.effort != providers::Effort::Default;
                const bool thinking = settings.thinking != providers::Thinking::Default;
                // Name only the kinds of setting that were sent, and how to send none of them.
                std::string which;
                std::string without;
                if (effort && thinking) {
                    which = "effort or thinking setting";
                    without = "with --effort default and without --thinking (then no effort or "
                              "thinking setting is sent)";
                } else if (effort) {
                    which = "effort setting";
                    without = "with --effort default (then no effort setting is sent)";
                } else {
                    which = "thinking setting";
                    without = "without --thinking (then no thinking setting is sent)";
                }
                // The note on thinking values applies only to the two that some models refuse.
                if (settings.thinking == providers::Thinking::Disabled ||
                    settings.thinking == providers::Thinking::BetweenTools) {
                    which += " (thinking disabled or between_tools works only on some models, and "
                             "only at effort high or below)";
                }
                advice.emplace_back(
                    "The request carried " + carried + ". The model may not support that " + which +
                    ". Run the same check " + without + " to see whether that is the cause.");
            }
        }
        if (!error.message.empty()) {
            advice.emplace_back("The service said: " + error.message);
        }
        break;
    case Code::RateLimit:
        advice.emplace_back(status + (status.empty() ? "" : ": ") +
                            "too many requests, or the account's limit was reached. Wait a minute "
                            "and run the check again; if it keeps happening, look at the usage and "
                            "limits page of your account.");
        if (error.retryAfterSeconds.has_value()) {
            advice.emplace_back("The service asked to wait " +
                                std::to_string(*error.retryAfterSeconds) + " seconds.");
        }
        break;
    case Code::Overloaded:
        advice.emplace_back(status + (status.empty() ? "" : ": ") +
                            "the service is busy. This is not about your key or your setup; try "
                            "again in a few minutes.");
        break;
    case Code::Network:
        advice.emplace_back("The service could not be reached. Check the internet connection, and "
                            "any VPN, proxy or firewall that this computer uses.");
        if (!error.message.empty()) {
            advice.emplace_back("Detail: " + error.message);
        }
        break;
    case Code::Policy:
        advice.emplace_back(
            "The service declined to answer the test question. Run the check again.");
        break;
    case Code::Cancelled:
        advice.emplace_back("The check was stopped.");
        break;
    case Code::Other:
        advice.emplace_back(
            status + (status.empty() ? "" : ": ") +
            "something went wrong on the service side. Try again in a few minutes.");
        if (!error.message.empty()) {
            advice.emplace_back("Detail: " + error.message);
        }
        break;
    }
    return advice;
}

std::string cacheProbeText(std::size_t bytes) {
    std::string text =
        "This block is filler for a connection check. It carries no information; the only "
        "request is to ignore it.\n";
    std::size_t n = 0;
    constexpr std::size_t kWords = sizeof(kFillerWords) / sizeof(kFillerWords[0]);
    while (text.size() < bytes) {
        text += "Filler line " + std::to_string(n) + ": the " + kFillerWords[n % kWords] +
                " near the " + kFillerWords[(n * 7 + 3) % kWords] + " keeps " +
                std::to_string((n * 37) % 991) + " measures of " +
                kFillerWords[(n * 5 + 1) % kWords] + ".\n";
        ++n;
    }
    text.resize(bytes);
    return text;
}

CacheProbeVerdict judgeCacheProbe(const providers::Usage& first, const providers::Usage& second) {
    CacheProbeVerdict verdict;
    verdict.prefixTokens = first.input + first.cacheRead + first.cacheWrite;
    const std::string numbers = "first: input=" + std::to_string(first.input) +
                                " cache_write=" + std::to_string(first.cacheWrite) +
                                " cache_read=" + std::to_string(first.cacheRead) +
                                "; second: input=" + std::to_string(second.input) +
                                " cache_write=" + std::to_string(second.cacheWrite) +
                                " cache_read=" + std::to_string(second.cacheRead);
    if (second.cacheRead > 0) {
        verdict.result = "hit";
        verdict.lines.emplace_back("cache hit: the first request wrote " +
                                   std::to_string(first.cacheWrite) +
                                   " tokens to the cache and the second read " +
                                   std::to_string(second.cacheRead) + " of them back");
        verdict.lines.emplace_back(numbers);
        return verdict;
    }
    if (first.cacheWrite > 0) {
        verdict.result = "written_not_read";
        verdict.lines.emplace_back(
            "cache written but not read back: the first request wrote " +
            std::to_string(first.cacheWrite) +
            " tokens and the second read none. Run the check again in a minute; if it repeats, "
            "send these numbers back");
        verdict.lines.emplace_back(numbers);
        return verdict;
    }
    verdict.result = "miss";
    verdict.lines.emplace_back(
        "no cache hit: prefix below the model minimum or caching not applied");
    verdict.lines.emplace_back(
        "the first request had " + std::to_string(verdict.prefixTokens) +
        " tokens in total. If that is below the minimum of the model (about 1,000 to 4,000 "
        "tokens, depending on the model), the prompt was too short to be cached; if it is "
        "well above, caching is not being applied. Send these numbers back");
    verdict.lines.emplace_back(numbers);
    return verdict;
}

}  // namespace curia
