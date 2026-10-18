// SPDX-License-Identifier: MIT
// `curia --check-api`: the owner's preflight. One tiny request (and, with --cache-probe, two
// identical long ones) through the same provider the app uses; the result is worded by
// core/check_api_report.*. The key is never printed, nor is any request body.
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "core/check_api_report.h"
#include "core/dev_config.h"
#include "headless/headless_modes.h"
#include "http/anthropic_provider.h"
#include "http/tls_policy.h"
#include "net/endpoint_policy.h"
#include "providers/cancel_token.h"

namespace curia::headless {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kRequestTimeoutSeconds = 120;
constexpr int kNetSummaryWaitMs = 3000;
constexpr int kExitOk = 0;
constexpr int kExitFailed = 1;
constexpr int kExitSetup = 2;
constexpr int kExitTimeout = 3;

// What the net thread reports; shared with the provider's options.
struct NetCollector {
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<http::NetSummary> summaries;
};

// What one request came to.
struct Outcome {
    bool timedOut = false;
    bool succeeded = false;
    providers::ChatError error;
    providers::Usage usage;
    long long firstTokenMs = -1;
    long long totalMs = -1;
    std::size_t textBytes = 0;
    std::optional<http::NetSummary> net;
};

struct RequestState {
    std::mutex mutex;
    std::condition_variable cv;
    bool finished = false;
    bool succeeded = false;
    providers::ChatError error;
    providers::Usage usage;
    std::optional<Clock::time_point> firstToken;
    std::size_t textBytes = 0;
    Clock::time_point end;
};

long long millis(Clock::time_point from, Clock::time_point to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

Outcome runRequest(http::AnthropicProvider& provider, const providers::ChatRequest& request,
                   NetCollector& collector) {
    std::size_t netBefore = 0;
    {
        const std::lock_guard<std::mutex> lock(collector.mutex);
        netBefore = collector.summaries.size();
    }
    auto state = std::make_shared<RequestState>();
    providers::StreamHandlers handlers;
    handlers.onToken = [state](providers::TokenKind kind, std::string_view text) {
        if (kind != providers::TokenKind::Text) {
            return;
        }
        const std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->firstToken.has_value()) {
            state->firstToken = Clock::now();
        }
        state->textBytes += text.size();
    };
    handlers.onDone = [state](const providers::Usage& usage) {
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->finished = true;
            state->succeeded = true;
            state->usage = usage;
            state->end = Clock::now();
        }
        state->cv.notify_all();
    };
    handlers.onError = [state](const providers::ChatError& error) {
        {
            const std::lock_guard<std::mutex> lock(state->mutex);
            state->finished = true;
            state->succeeded = false;
            state->error = error;
            state->end = Clock::now();
        }
        state->cv.notify_all();
    };

    providers::CancelToken cancel;
    const Clock::time_point start = Clock::now();
    provider.streamChat(request, std::move(handlers), cancel);

    Outcome outcome;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        if (!state->cv.wait_for(lock, std::chrono::seconds(kRequestTimeoutSeconds),
                                [&] { return state->finished; })) {
            outcome.timedOut = true;
            lock.unlock();
            cancel.cancel();
            return outcome;
        }
        outcome.succeeded = state->succeeded;
        outcome.error = state->error;
        outcome.usage = state->usage;
        outcome.textBytes = state->textBytes;
        outcome.totalMs = millis(start, state->end);
        if (state->firstToken.has_value()) {
            outcome.firstTokenMs = millis(start, *state->firstToken);
        }
    }
    // The connection's own numbers arrive when it has closed, just after the last event.
    std::unique_lock<std::mutex> lock(collector.mutex);
    if (collector.cv.wait_for(lock, std::chrono::milliseconds(kNetSummaryWaitMs),
                              [&] { return collector.summaries.size() > netBefore; })) {
        outcome.net = collector.summaries[netBefore];
    }
    return outcome;
}

std::string number(long long value) {
    return std::to_string(value);
}

// The answer limit of the check's requests: room for "OK" and a little thinking. Thinking counts
// toward it, so the model's thinking can use it up, more likely at a high effort (the check then
// says so).
constexpr int kCheckMaxTokens = 256;

providers::ChatRequest tinyRequest(const std::string& model, const CliOptions& options) {
    providers::ChatRequest request;
    request.model = model;
    request.effort = options.effort;
    request.thinking = options.thinking;
    request.systemPersona =
        "This is a connection check from a developer tool. Reply with the single word OK.";
    request.cache.cacheSystem = false;
    request.cache.cacheSnapshot = false;
    request.conversation.push_back({providers::Role::User, "Please reply with OK."});
    request.maxTokens = kCheckMaxTokens;
    return request;
}

providers::ChatRequest cacheRequest(const std::string& model, const CliOptions& options) {
    providers::ChatRequest request;
    request.model = model;
    request.effort = options.effort;
    request.thinking = options.thinking;
    request.systemPersona = cacheProbeText();
    request.cache.cacheSystem = true;
    request.cache.cacheSnapshot = false;
    request.conversation.push_back({providers::Role::User, "Please reply with OK."});
    request.maxTokens = kCheckMaxTokens;
    return request;
}

std::string caSource(const http::NetSummary& net) {
    if (net.tlsRetried) {
        const std::optional<std::string> bundle = http::TlsPolicy::system().probedBundle();
        return "the fallback CA bundle " + bundle.value_or(std::string("(none found)")) +
               " (the system default store did not trust the connection)";
    }
    return "the system default certificate store";
}

}  // namespace

int runCheckApiMode(const CliOptions& options, std::ostream& out, std::ostream& err) {
    DevConfig config = loadDevConfig();
    CheckApiReport report;
    const bool keyOk = config.apiKey.has_value();
    const bool modelOk = config.model.has_value();

    for (const std::string& warning : config.warnings) {
        err << "curia: note: " << warning << '\n';
    }
    {
        CheckStep step;
        step.name = "API key";
        step.state = keyOk ? CheckStep::State::Ok : CheckStep::State::Failed;
        step.detail = keyOk ? "present (never shown)" : "missing or not usable";
        if (!keyOk) {
            step.advice = adviceForMissing({std::string(keyVariableName())});
        }
        report.add(std::move(step));
    }
    {
        CheckStep step;
        step.name = "Model";
        step.state = modelOk ? CheckStep::State::Ok : CheckStep::State::Failed;
        step.detail = modelOk ? *config.model : "missing or not usable";
        if (!modelOk) {
            step.advice = adviceForMissing({std::string(modelVariableName())});
        }
        report.add(std::move(step));
    }
    // Every request of the check carries the settings the app would send (Curia's default effort
    // unless a flag says otherwise), so a model that refuses them is found here.
    const SentSettings settings = sentSettings(options);
    const std::string carried = describeSettings(settings);
    if (!carried.empty()) {
        CheckStep step;
        step.name = "Effort and thinking";
        step.state = CheckStep::State::Info;
        step.detail = "every request of this check carries " + carried;
        report.add(std::move(step));
    }
    report.set("key", keyOk ? "present" : "missing");
    report.set("model", modelOk ? "set" : "missing");
    report.set("effort", providers::toString(options.effort));
    report.set("thinking", providers::toString(options.thinking));
    if (!keyOk || !modelOk) {
        report.set("tls", "skipped");
        report.set("auth", authName(options.auth));
        report.set("completion", "skipped");
        out << formatCheckApiReport(report) << std::flush;
        return kExitSetup;
    }

    net::Endpoint endpoint;
    std::string endpointError;
    std::optional<std::string_view> flag;
    if (!options.testEndpoint.empty()) {
        flag = std::string_view(options.testEndpoint);
    }
    if (!net::makeEndpoint(flag, &*config.apiKey, endpoint, endpointError)) {
        CheckStep step;
        step.name = "Address";
        step.state = CheckStep::State::Failed;
        step.detail = endpointError;
        report.add(std::move(step));
        report.set("tls", "skipped");
        report.set("auth", authName(options.auth));
        report.set("completion", "skipped");
        out << formatCheckApiReport(report) << std::flush;
        return kExitSetup;
    }
    const bool testEndpoint = endpoint.isTest;

    std::unique_ptr<AppLog> log = openAppLog(options);
    auto collector = std::make_shared<NetCollector>();
    http::AnthropicProviderOptions providerOptions;
    providerOptions.auth = toAuthStyle(options.auth);
    providerOptions.log = log.get();
    providerOptions.onNetDone = [collector](const http::NetSummary& summary) {
        {
            const std::lock_guard<std::mutex> lock(collector->mutex);
            collector->summaries.push_back(summary);
        }
        collector->cv.notify_all();
    };
    const std::string model = *config.model;
    auto provider = std::make_unique<http::AnthropicProvider>(
        std::move(endpoint), std::move(*config.apiKey), providerOptions);

    out << "Sending one tiny request (usually a few seconds)..." << '\n' << std::flush;
    const Outcome first = runRequest(*provider, tinyRequest(model, options), *collector);
    if (first.timedOut) {
        out << "No answer within " << kRequestTimeoutSeconds << " seconds. The check was stopped.\n"
            << "CHECK_API_RESULT ok=false timeout=true effort="
            << providers::toString(options.effort)
            << " thinking=" << providers::toString(options.thinking) << '\n'
            << std::flush;
        return kExitTimeout;
    }

    const int status = first.net.has_value() ? first.net->httpStatus
                                             : (first.succeeded ? 200 : first.error.httpStatus);
    const bool reachedServer = status > 0;

    {
        CheckStep step;
        step.name = "Connection and certificates";
        if (testEndpoint) {
            step.state = reachedServer ? CheckStep::State::Info : CheckStep::State::Failed;
            step.detail = "plain http to a test server on this computer; certificates not tested";
        } else if (reachedServer) {
            step.state = CheckStep::State::Ok;
            step.detail = first.net.has_value()
                              ? "connected in " + number(first.net->connectMs) +
                                    " ms, certificates checked with " + caSource(*first.net)
                              : std::string("connected (the connection's own numbers were not "
                                            "reported)");
        } else {
            step.state = CheckStep::State::Failed;
            step.detail = "could not connect";
            step.advice = adviceForError(first.error, options.auth, settings);
        }
        report.add(std::move(step));
    }
    report.set("tls", testEndpoint ? "not_applicable" : (reachedServer ? "ok" : "failed"));
    report.set("ca", testEndpoint || !first.net.has_value() || !reachedServer
                         ? "na"
                         : (first.net->tlsRetried ? "fallback" : "default"));

    {
        CheckStep step;
        step.name = std::string("Sign-in header (") + authName(options.auth) + ")";
        if (!reachedServer) {
            step.state = CheckStep::State::Skipped;
            step.detail = "no answer from the service";
        } else if (status == 401 || status == 403) {
            step.state = CheckStep::State::Failed;
            step.detail = "rejected (HTTP " + number(status) + ")";
            step.advice = adviceForError(first.error, options.auth, settings);
        } else if (status >= 200 && status < 300) {
            step.state = CheckStep::State::Ok;
            step.detail = "accepted (HTTP " + number(status) + ")";
        } else {
            step.state = CheckStep::State::Info;
            step.detail = "not rejected; the service answered HTTP " + number(status);
        }
        report.add(std::move(step));
    }
    report.set("auth", authName(options.auth));
    report.set("status", number(status));

    {
        CheckStep step;
        step.name = "Tiny answer";
        if (first.succeeded) {
            step.state = CheckStep::State::Ok;
            step.detail = "first token after " + number(first.firstTokenMs) +
                          " ms, finished after " + number(first.totalMs) + " ms; tokens in " +
                          number(first.usage.input) + ", out " + number(first.usage.output) +
                          ", stop reason " +
                          (first.usage.stopReason.empty() ? "none" : first.usage.stopReason);
            if (first.textBytes == 0) {
                step.detail += " (no text came back: the small test budget may have been used up "
                               "before the answer; not a problem for the connection)";
                // Not when the thinking before the answer was turned off: "disabled", and
                // "between_tools" in a request without tools (research-notes C13).
                const bool thinksFirst = settings.thinking == providers::Thinking::Default ||
                                         settings.thinking == providers::Thinking::Adaptive;
                if (!carried.empty() && thinksFirst) {
                    step.advice.push_back(
                        "The request carried " + carried + ". Thinking counts toward the check's " +
                        number(kCheckMaxTokens) +
                        "-token answer limit, so the model's thinking can use it up before the "
                        "answer (more likely at a high effort).");
                }
            }
        } else {
            step.state = CheckStep::State::Failed;
            step.detail = "no answer";
            if (step.advice.empty()) {
                step.advice = adviceForError(first.error, options.auth, settings);
            }
        }
        report.add(std::move(step));
    }
    report.set("completion", first.succeeded ? "ok" : "failed");
    report.set("first_token_ms", number(first.firstTokenMs));
    report.set("total_ms", number(first.totalMs));
    report.set("input", number(first.usage.input));
    report.set("output", number(first.usage.output));

    int exitCode = first.succeeded ? kExitOk : kExitFailed;
    if (options.cacheProbe) {
        if (!first.succeeded) {
            CheckStep step;
            step.name = "Prompt cache probe";
            step.state = CheckStep::State::Skipped;
            step.detail = "skipped because the tiny answer failed";
            report.add(std::move(step));
            report.set("cache_probe", "skipped");
        } else {
            out << "Cache probe: two identical requests with a long shared start..." << '\n'
                << std::flush;
            const providers::ChatRequest probe = cacheRequest(model, options);
            const Outcome a = runRequest(*provider, probe, *collector);
            Outcome b;
            if (a.succeeded) {
                b = runRequest(*provider, probe, *collector);
            }
            CheckStep step;
            step.name = "Prompt cache probe";
            if (a.timedOut || b.timedOut) {
                step.state = CheckStep::State::Failed;
                step.detail = "a request took longer than " + number(kRequestTimeoutSeconds) + " s";
                exitCode = kExitTimeout;
                report.set("cache_probe", "timeout");
            } else if (!a.succeeded || !b.succeeded) {
                step.state = CheckStep::State::Failed;
                step.detail = "a probe request failed";
                step.advice =
                    adviceForError(!a.succeeded ? a.error : b.error, options.auth, settings);
                exitCode = kExitFailed;
                report.set("cache_probe", "failed");
            } else {
                const CacheProbeVerdict verdict = judgeCacheProbe(a.usage, b.usage);
                step.state = CheckStep::State::Info;
                step.detail = verdict.lines.empty() ? verdict.result : verdict.lines.front();
                for (std::size_t i = 1; i < verdict.lines.size(); ++i) {
                    step.advice.push_back(verdict.lines[i]);
                }
                report.set("cache_probe", verdict.result);
                report.set("cache_write", number(a.usage.cacheWrite));
                report.set("cache_read", number(b.usage.cacheRead));
                report.set("prefix_tokens", number(verdict.prefixTokens));
            }
            report.add(std::move(step));
        }
    }
    if (!report.passed() && exitCode == kExitOk) {
        exitCode = kExitFailed;
    }
    out << formatCheckApiReport(report) << std::flush;
    return exitCode;
}

}  // namespace curia::headless
