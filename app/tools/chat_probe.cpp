// SPDX-License-Identifier: MIT
// A small command-line client of the Anthropic provider, for tests and the owner's preflight: it
// sends one question and prints what happens as one JSON object per line on stdout.
//
//   {"event":"token","kind":"text","text":"..."}
//   {"event":"done","usage":{"input":..,"output":..,"cache_read":..,"cache_write":..},
//    "first_token_ms":..,"total_ms":..,"stop_reason":".."}
//   {"event":"error","code":"Network","message":"..","http_status":0,"partial_len":0,
//    "partial_text":"..",...}
//   {"event":"destroyed","late_callbacks":0}      (only with --destroy-after-tokens)
//   {"event":"lingering"}                          (only with --linger-ms, see below)
//
// Exit code: 0 the answer is complete, 1 the request failed or was cancelled, 2 bad command line
// or configuration (nothing was sent), 3 the probe's own overall timeout.
//
// --linger-ms N keeps the process alive for N ms after the request has ended, with the provider
// still alive (after --destroy-after-tokens: with the provider already gone), and prints
// {"event":"lingering"} when that starts. A test uses it to prove that a cancelled or destroyed
// request has closed its connection itself and not merely through the process exiting.
//
// The key is read exactly as the app reads it (core/dev_config.h: the process environment, in a
// build with CURIA_ENABLE_DEV_ENV) and is never printed: the provider redacts the tokens and the
// error texts it hands over, and the probe adds nothing of its own.
// --test-endpoint (same build only) points the probe at a mock server on loopback and then needs a
// key that contains FAKE (net/endpoint_policy.h). Without it the probe talks to the real API.
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#include <nlohmann/json.hpp>

#include "core/app_log.h"
#include "core/dev_config.h"
#include "core/redact.h"
#include "http/anthropic_provider.h"
#include "net/endpoint_policy.h"
#include "providers/cancel_token.h"
#include "providers/chat_types.h"

namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr int kExitDone = 0;
constexpr int kExitChatError = 1;
constexpr int kExitUsage = 2;
constexpr int kExitTimeout = 3;

struct Options {
    std::optional<std::string> testEndpoint;
    std::optional<std::string> model;
    std::optional<std::string> prompt;
    std::string persona;
    std::string snapshot;
    curia::http::AuthStyle auth = curia::http::AuthStyle::Bearer;
    std::optional<long long> stallTimeoutMs;
    std::optional<long long> connectTimeoutMs;
    long long cancelAfterTokens = 0;
    long long cancelAfterMs = 0;
    long long destroyAfterTokens = 0;
    long long overallTimeoutMs = 120000;
    long long lingerMs = 0;
    int maxTokens = 256;
    bool showThinking = false;
    std::optional<std::string> logDir;
};

void printUsage() {
    std::fputs(
        "usage: curia_chat_probe [--test-endpoint http://127.0.0.1:PORT] [--model NAME]\n"
        "                        [--prompt TEXT] [--persona TEXT] [--snapshot TEXT]\n"
        "                        [--auth bearer|x-api-key] [--max-tokens N] [--show-thinking]\n"
        "                        [--stall-timeout-ms N] [--connect-timeout-ms N]\n"
        "                        [--cancel-after-tokens N] [--cancel-after-ms N]\n"
        "                        [--destroy-after-tokens N] [--overall-timeout-ms N]\n"
        "                        [--linger-ms N]\n"
        "                        [--log-dir DIR]\n"
        "The question is read from stdin when --prompt is not given. The API key and the default\n"
        "model come from the development configuration of the build (see the app's "
        "documentation).\n",
        stderr);
}

bool parseNumber(const std::string& text, long long minimum, long long maximum, long long& out) {
    if (text.empty() || text.size() > 12) {
        return false;
    }
    long long value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        value = value * 10 + (ch - '0');
    }
    if (value < minimum || value > maximum) {
        return false;
    }
    out = value;
    return true;
}

bool validModelName(const std::string& name) {
    if (name.empty() || name.size() > 128) {
        return false;
    }
    for (const char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '.' || ch == '_' || ch == ':' ||
                        ch == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool isValueFlag(const std::string& flag) {
    for (const char* known :
         {"--test-endpoint", "--model", "--prompt", "--persona", "--snapshot", "--auth",
          "--log-dir", "--stall-timeout-ms", "--connect-timeout-ms", "--cancel-after-tokens",
          "--cancel-after-ms", "--destroy-after-tokens", "--overall-timeout-ms", "--max-tokens",
          "--linger-ms"}) {
        if (flag == known) {
            return true;
        }
    }
    return false;
}

// Returns an empty string on success, else a message that never repeats a value.
std::string parseArguments(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        auto next = [&](std::string& value) {
            if (i + 1 >= argc) {
                return false;
            }
            value = argv[++i];
            return true;
        };
        std::string value;
        long long number = 0;
        if (flag == "--show-thinking") {
            options.showThinking = true;
            continue;
        }
        if (flag == "--help" || flag == "-h") {
            return "help";
        }
        if (!isValueFlag(flag)) {
            return "unknown option " + flag;
        }
        if (!next(value)) {
            return "missing value for " + flag;
        }
        if (flag == "--test-endpoint") {
            options.testEndpoint = value;
        } else if (flag == "--model") {
            if (!validModelName(value)) {
                return "--model must be 1 to 128 characters from letters, digits and . _ : -";
            }
            options.model = value;
        } else if (flag == "--prompt") {
            options.prompt = value;
        } else if (flag == "--persona") {
            options.persona = value;
        } else if (flag == "--snapshot") {
            options.snapshot = value;
        } else if (flag == "--auth") {
            if (value == "bearer") {
                options.auth = curia::http::AuthStyle::Bearer;
            } else if (value == "x-api-key") {
                options.auth = curia::http::AuthStyle::XApiKey;
            } else {
                return "--auth must be bearer or x-api-key";
            }
        } else if (flag == "--log-dir") {
            options.logDir = value;
        } else if (flag == "--stall-timeout-ms" || flag == "--connect-timeout-ms" ||
                   flag == "--cancel-after-tokens" || flag == "--cancel-after-ms" ||
                   flag == "--destroy-after-tokens" || flag == "--overall-timeout-ms" ||
                   flag == "--max-tokens" || flag == "--linger-ms") {
            if (!parseNumber(value, 1, 3600000, number)) {
                return flag + " needs a whole number from 1 to 3600000";
            }
            if (flag == "--stall-timeout-ms") {
                options.stallTimeoutMs = number;
            } else if (flag == "--connect-timeout-ms") {
                options.connectTimeoutMs = number;
            } else if (flag == "--cancel-after-tokens") {
                options.cancelAfterTokens = number;
            } else if (flag == "--cancel-after-ms") {
                options.cancelAfterMs = number;
            } else if (flag == "--destroy-after-tokens") {
                options.destroyAfterTokens = number;
            } else if (flag == "--overall-timeout-ms") {
                options.overallTimeoutMs = number;
            } else if (flag == "--linger-ms") {
                options.lingerMs = number;
            } else {
                options.maxTokens = static_cast<int>(number);
            }
        }
    }
    return {};
}

// What the handlers (on the net thread) and the main thread share. Held by shared_ptr so that a
// handler that runs after main() has moved on still has something valid to touch.
struct Shared {
    std::mutex outMutex;
    std::mutex stateMutex;
    std::condition_variable changed;
    bool finished = false;
    int exitCode = kExitDone;
    bool destroyRequested = false;
    std::atomic<bool> providerGone{false};
    std::atomic<int> lateCallbacks{0};
    long long tokens = 0;
    std::optional<long long> firstTokenMs;
    Clock::time_point start;
    long long cancelAfterTokens = 0;
    long long destroyAfterTokens = 0;
    curia::providers::CancelToken cancel;

    // One line. Nothing is redacted here on purpose: the probe prints exactly what the provider
    // hands over, so that the tests prove the provider's own redaction.
    void emit(const json& event) {
        std::string line = event.dump(-1, ' ', false, json::error_handler_t::replace);
        line.push_back('\n');
        const std::lock_guard<std::mutex> lock(outMutex);
        std::fwrite(line.data(), 1, line.size(), stdout);
        std::fflush(stdout);
    }

    void finish(int code) {
        {
            const std::lock_guard<std::mutex> lock(stateMutex);
            finished = true;
            exitCode = code;
        }
        changed.notify_all();
    }

    long long elapsedMs() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
    }
};

void emitSetupError(const std::string& message) {
    const json event = {{"event", "error"},
                        {"code", "Config"},
                        {"message", curia::redact(message)},
                        {"http_status", 0},
                        {"partial_len", 0}};
    std::fputs((event.dump() + "\n").c_str(), stdout);
    std::fflush(stdout);
}

void lingerFor(Shared& shared, long long ms) {
    if (ms <= 0) {
        return;
    }
    shared.emit({{"event", "lingering"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string readAllStdin() {
    return std::string(std::istreambuf_iterator<char>(std::cin), std::istreambuf_iterator<char>());
}

}  // namespace

int main(int argc, char** argv) {
    Options options;
    const std::string argumentProblem = parseArguments(argc, argv, options);
    if (!argumentProblem.empty()) {
        if (argumentProblem != "help") {
            std::fprintf(stderr, "curia_chat_probe: %s\n", argumentProblem.c_str());
        }
        printUsage();
        return argumentProblem == "help" ? kExitDone : kExitUsage;
    }

    curia::DevConfig config = curia::loadDevConfig();
    if (!config.apiKey.has_value()) {
        emitSetupError(curia::describeNotConfigured(config));
        return kExitUsage;
    }
    std::string model;
    if (options.model.has_value()) {
        model = *options.model;
    } else if (config.model.has_value()) {
        model = *config.model;
    } else {
        emitSetupError(curia::describeNotConfigured(config));
        return kExitUsage;
    }

    std::optional<std::string_view> endpointFlag;
    if (options.testEndpoint.has_value()) {
        endpointFlag = std::string_view(*options.testEndpoint);
    }
    curia::net::Endpoint endpoint;
    std::string endpointError;
    if (!curia::net::makeEndpoint(endpointFlag, &*config.apiKey, endpoint, endpointError)) {
        emitSetupError(endpointError);
        return kExitUsage;
    }

    std::string question = options.prompt.has_value() ? *options.prompt : readAllStdin();
    if (question.empty()) {
        emitSetupError("there is no question: give --prompt or write it to stdin");
        return kExitUsage;
    }

    std::unique_ptr<curia::AppLog> appLog;
    if (options.logDir.has_value()) {
        appLog = std::make_unique<curia::AppLog>(std::filesystem::path(*options.logDir));
    }

    curia::http::AnthropicProviderOptions providerOptions;
    providerOptions.auth = options.auth;
    providerOptions.log = appLog.get();
    if (options.stallTimeoutMs.has_value()) {
        providerOptions.http.stallTimeout = std::chrono::milliseconds(*options.stallTimeoutMs);
    }
    if (options.connectTimeoutMs.has_value()) {
        providerOptions.http.connectTimeout = std::chrono::milliseconds(*options.connectTimeoutMs);
    }

    auto provider = std::make_unique<curia::http::AnthropicProvider>(
        endpoint, std::move(*config.apiKey), providerOptions);

    curia::providers::ChatRequest request;
    request.model = model;
    request.systemPersona = options.persona;
    request.snapshotText = options.snapshot;
    request.maxTokens = options.maxTokens;
    request.showThinking = options.showThinking;
    request.conversation.push_back({curia::providers::Role::User, std::move(question)});

    auto shared = std::make_shared<Shared>();
    shared->cancelAfterTokens = options.cancelAfterTokens;
    shared->destroyAfterTokens = options.destroyAfterTokens;

    curia::providers::StreamHandlers handlers;
    handlers.onToken = [shared](curia::providers::TokenKind kind, std::string_view text) {
        if (shared->providerGone.load()) {
            ++shared->lateCallbacks;
            return;
        }
        ++shared->tokens;
        if (kind == curia::providers::TokenKind::Text && !shared->firstTokenMs.has_value()) {
            shared->firstTokenMs = shared->elapsedMs();
        }
        shared->emit({{"event", "token"},
                      {"kind", kind == curia::providers::TokenKind::Text ? "text" : "thinking"},
                      {"text", std::string(text)}});
        if (shared->cancelAfterTokens > 0 && shared->tokens == shared->cancelAfterTokens) {
            shared->cancel.cancel();
        }
        if (shared->destroyAfterTokens > 0 && shared->tokens == shared->destroyAfterTokens) {
            {
                const std::lock_guard<std::mutex> lock(shared->stateMutex);
                shared->destroyRequested = true;
            }
            shared->changed.notify_all();
        }
    };
    handlers.onDone = [shared](const curia::providers::Usage& usage) {
        if (shared->providerGone.load()) {
            ++shared->lateCallbacks;
            return;
        }
        json event = {{"event", "done"},
                      {"usage",
                       {{"input", usage.input},
                        {"output", usage.output},
                        {"cache_read", usage.cacheRead},
                        {"cache_write", usage.cacheWrite}}},
                      {"first_token_ms", shared->firstTokenMs.has_value()
                                             ? json(*shared->firstTokenMs)
                                             : json(nullptr)},
                      {"total_ms", shared->elapsedMs()},
                      {"stop_reason", usage.stopReason}};
        shared->emit(event);
        shared->finish(kExitDone);
    };
    handlers.onError = [shared](const curia::providers::ChatError& error) {
        if (shared->providerGone.load()) {
            ++shared->lateCallbacks;
            return;
        }
        json event = {{"event", "error"},
                      {"code", curia::providers::toString(error.code)},
                      {"message", error.message},
                      {"http_status", error.httpStatus},
                      {"retryable", error.retryable},
                      {"retry_after", error.retryAfterSeconds.has_value()
                                          ? json(*error.retryAfterSeconds)
                                          : json(nullptr)},
                      {"request_id", error.requestId},
                      {"partial_len", error.partialText.size()},
                      {"partial_text", error.partialText},
                      {"total_ms", shared->elapsedMs()}};
        shared->emit(event);
        shared->finish(kExitChatError);
    };

    shared->start = Clock::now();
    provider->streamChat(request, std::move(handlers), shared->cancel);

    std::unique_lock<std::mutex> lock(shared->stateMutex);
    const auto deadline = shared->start + std::chrono::milliseconds(options.overallTimeoutMs);
    auto ended = [&] { return shared->finished || shared->destroyRequested; };
    if (options.cancelAfterMs > 0) {
        const auto cancelAt = shared->start + std::chrono::milliseconds(options.cancelAfterMs);
        if (!shared->changed.wait_until(lock, cancelAt, ended)) {
            lock.unlock();
            shared->cancel.cancel();
            lock.lock();
        }
    }
    if (!shared->changed.wait_until(lock, deadline, ended)) {
        lock.unlock();
        shared->cancel.cancel();
        provider.reset();
        shared->providerGone.store(true);
        shared->emit({{"event", "error"},
                      {"code", "Timeout"},
                      {"message", "the probe's overall timeout expired"},
                      {"http_status", 0},
                      {"partial_len", 0}});
        return kExitTimeout;
    }
    if (shared->destroyRequested && !shared->finished) {
        lock.unlock();
        provider.reset();  // joins the net thread: no handler may run after this returns
        shared->providerGone.store(true);
        lingerFor(*shared, options.lingerMs);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        shared->emit({{"event", "destroyed"}, {"late_callbacks", shared->lateCallbacks.load()}});
        return shared->lateCallbacks.load() == 0 ? kExitDone : kExitChatError;
    }
    const int code = shared->exitCode;
    lock.unlock();
    lingerFor(*shared, options.lingerMs);
    provider.reset();
    return code;
}
