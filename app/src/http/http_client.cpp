// SPDX-License-Identifier: MIT
#include "http/http_client.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#include "core/redact.h"
#include "http/header_capture.h"
#include "providers/provider_util.h"

namespace curia::http {

namespace {

using Clock = std::chrono::steady_clock;

constexpr long kMaxConnectTimeoutMs = 10 * 60 * 1000;
constexpr long long kMaxStallMs = 3600LL * 1000;
// Never block the net thread for longer than this with transfers in flight: a safety net for a
// cancel whose waker was replaced (a token shared between two clients) and for the stall check.
constexpr int kBusyPollMs = 1000;
constexpr int kIdlePollMs = 30000;
// TCP keep-alive: probes keep a NAT mapping alive during the long silent phase before the first
// token of a thinking model, and find a dead peer.
constexpr long kKeepAliveIdleSeconds = 30;
constexpr long kKeepAliveIntervalSeconds = 15;

void ensureCurlInitialised() {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

// "scheme://authority" of a URL that has a path, or empty.
std::string_view originOf(std::string_view url) {
    const std::size_t scheme = url.find("://");
    if (scheme == std::string_view::npos) {
        return {};
    }
    const std::size_t slash = url.find('/', scheme + 3);
    if (slash == std::string_view::npos) {
        return {};
    }
    return url.substr(0, slash);
}

bool hasStartingScheme(std::string_view url, std::string_view scheme) {
    return url.substr(0, scheme.size()) == scheme;
}

bool urlHasBadByte(std::string_view url) {
    for (const char ch : url) {
        const auto byte = static_cast<unsigned char>(ch);
        if (byte <= 0x20 || byte >= 0x7f || ch == '\\') {
            return true;
        }
    }
    return false;
}

bool validHeaderName(std::string_view name) {
    if (name.empty()) {
        return false;
    }
    for (const char ch : name) {
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                        (ch >= '0' && ch <= '9') || ch == '-' || ch == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool validHeaderValue(std::string_view value) {
    return value.find_first_of(std::string_view("\r\n\0", 3)) == std::string_view::npos;
}

std::int64_t millisSince(Clock::time_point start, Clock::time_point now) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
}

struct WakeHandle {
    std::mutex mutex;
    CURLM* multi = nullptr;

    // Safe from any thread, also after the client is gone (then it does nothing). The tokens of
    // in-flight requests keep this small object alive, never the multi handle.
    void wake() {
        const std::lock_guard<std::mutex> lock(mutex);
        if (multi != nullptr) {
            curl_multi_wakeup(multi);
        }
    }
    void detach() {
        const std::lock_guard<std::mutex> lock(mutex);
        multi = nullptr;
    }
};

}  // namespace

Header Header::plain(std::string name, std::string value) {
    Header header;
    header.name = std::move(name);
    header.value = std::move(value);
    return header;
}

Header Header::withSecret(std::string name, std::string prefix,
                          std::shared_ptr<const SecretString> secret) {
    Header header;
    header.name = std::move(name);
    header.value = std::move(prefix);
    header.secret = std::move(secret);
    return header;
}

TransferPlan planTransfer(std::string_view url, const net::Endpoint& endpoint, bool carriesSecret,
                          const ClientOptions& options) {
    TransferPlan plan;
    const auto connectMs = std::clamp<long long>(options.connectTimeout.count(), 1,
                                                 static_cast<long long>(kMaxConnectTimeoutMs));
    plan.connectTimeoutMs = static_cast<long>(connectMs);
    plan.stallMs = std::clamp<long long>(options.stallTimeout.count(), 1, kMaxStallMs);
    plan.stallSeconds = static_cast<long>(std::max<long long>((plan.stallMs + 999) / 1000, 1));
    if (url.empty() || endpoint.url.empty()) {
        plan.refusal = "the request has no address";
        return plan;
    }
    if (urlHasBadByte(url)) {
        plan.refusal = "the request address contains a blank, control, backslash or non-ASCII byte";
        return plan;
    }
    // The endpoint is checked here, not trusted: whoever built it, plain http and "no proxy" apply
    // only to the loopback form of a development build, and https only to Anthropic's own URL.
    if (endpoint.isTest) {
        if (!net::isLoopbackTestUrl(endpoint.url)) {
            plan.refusal = "a test endpoint must be http://127.x.y.z:PORT or http://[::1]:PORT "
                           "(and only a development build accepts one)";
            return plan;
        }
    } else if (endpoint.url != net::kAnthropicMessagesUrl) {
        plan.refusal = "the endpoint is not Anthropic's";
        return plan;
    }
    const std::string_view origin = originOf(url);
    if (origin.empty() || origin != originOf(endpoint.url)) {
        plan.refusal = "the request address is not on the endpoint's origin";
        return plan;
    }
    // Only a test endpoint may use plain http, and then nothing else; a real endpoint is https
    // only.
    if (endpoint.isTest) {
        if (!hasStartingScheme(url, "http://")) {
            plan.refusal = "a test endpoint must use http";
            return plan;
        }
        plan.protocols = "http";
    } else {
        if (!hasStartingScheme(url, "https://")) {
            plan.refusal = "a real endpoint must use https";
            return plan;
        }
        plan.protocols = "https";
    }
    if (carriesSecret && !net::authMayAttach(url, endpoint)) {
        plan.refusal = "the API key may only be sent to the endpoint's own URL";
        return plan;
    }
    plan.disableProxy = endpoint.disableProxy || endpoint.isTest;
    return plan;
}

// --- the net thread ------------------------------------------------------------------------------

namespace {

struct Transfer {
    Request request;
    TransferPlan plan;
    CURL* easy = nullptr;
    curl_slist* headerList = nullptr;
    std::array<char, CURL_ERROR_SIZE> errorBuffer{};
    TlsPolicy::Attempt attempt;
    bool tlsRetried = false;

    HeaderCapture capture;
    bool headDelivered = false;
    bool cancelSeen = false;
    bool consumerAbort = false;
    bool gotFirstByte = false;
    bool stalled = false;
    std::uint64_t bytesIn = 0;
    Clock::time_point start;
    Clock::time_point firstByte;
    // The stall guard's reference: the start of the attempt, then every byte moved.
    Clock::time_point lastActivity;
    curl_off_t lastProgress = -1;
};

struct Item {
    std::unique_ptr<Transfer> transfer;
    std::function<void()> task;
};

// Zeroes the text of every node (a header line may hold the key) before the list is freed.
void freeHeaderList(curl_slist*& list) {
    for (curl_slist* node = list; node != nullptr; node = node->next) {
        if (node->data != nullptr) {
            secureZero(node->data, std::strlen(node->data));
        }
    }
    curl_slist_free_all(list);
    list = nullptr;
}

}  // namespace

struct HttpClient::Impl {
    ClientOptions options;
    std::shared_ptr<WakeHandle> wake = std::make_shared<WakeHandle>();
    CURLM* multi = nullptr;
    std::mutex mutex;
    std::deque<Item> pending;
    bool stopping = false;
    // An earlier request needed the located CA bundle: start with it from now on.
    std::atomic<bool> preferProbed{false};
    std::thread thread;
    // Net thread only.
    std::vector<std::unique_ptr<Transfer>> active;
    // The client was destroyed by a callback on the net thread: this object is freed when run()
    // ends. Written and read on the net thread only.
    bool selfOwned = false;

    explicit Impl(ClientOptions opts) : options(std::move(opts)) {}
    ~Impl() {
        wake->detach();
        if (multi != nullptr) {
            curl_multi_cleanup(multi);
            multi = nullptr;
        }
    }
    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    static std::size_t onWrite(char* data, std::size_t size, std::size_t count, void* user);
    static std::size_t onHeader(char* data, std::size_t size, std::size_t count, void* user);
    static int onProgress(void* user, curl_off_t, curl_off_t dlnow, curl_off_t, curl_off_t ulnow);

    void run();
    void startItem(Item item);
    void startTransfer(std::unique_ptr<Transfer> transfer);
    bool beginAttempt(Transfer& transfer, std::string& refusal, CURLcode& failure);
    bool buildHeaders(Transfer& transfer, std::string& refusal);
    void scanCancelled();
    int checkStalls();
    void drainMessages();
    void finish(Transfer* transfer, End end, CURLcode code);
    void complete(std::unique_ptr<Transfer> transfer, Result result);
    void shutdownAll();
};

std::size_t HttpClient::Impl::onWrite(char* data, std::size_t size, std::size_t count, void* user) {
    auto* transfer = static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    if (transfer->request.cancel.cancelled()) {
        transfer->cancelSeen = true;
        return 0;
    }
    transfer->lastActivity = Clock::now();
    if (!transfer->gotFirstByte) {
        transfer->gotFirstByte = true;
        transfer->firstByte = Clock::now();
    }
    transfer->bytesIn += bytes;
    const auto& onData = transfer->request.callbacks.onData;
    if (!onData) {
        return bytes;
    }
    bool keepGoing = false;
    try {
        keepGoing = onData(std::string_view(data, bytes));
    } catch (...) {
        keepGoing = false;
    }
    if (!keepGoing) {
        transfer->consumerAbort = true;
        return 0;
    }
    return bytes;
}

std::size_t HttpClient::Impl::onHeader(char* data, std::size_t size, std::size_t count,
                                       void* user) {
    auto* transfer = static_cast<Transfer*>(user);
    const std::size_t bytes = size * count;
    transfer->lastActivity = Clock::now();
    if (transfer->capture.feed(std::string_view(data, bytes)) && !transfer->headDelivered) {
        transfer->headDelivered = true;
        if (transfer->request.callbacks.onHead) {
            ResponseHead head;
            head.status = transfer->capture.status();
            head.requestId = transfer->capture.requestId();
            head.retryAfter = transfer->capture.retryAfter();
            try {
                transfer->request.callbacks.onHead(head);
            } catch (...) {
                transfer->consumerAbort = true;
                return 0;
            }
        }
    }
    return bytes;
}

// libcurl calls this about once a second and on every step with progress; it only watches the
// byte counters (upload and download) so that a slow upload does not look like silence.
int HttpClient::Impl::onProgress(void* user, curl_off_t, curl_off_t dlnow, curl_off_t,
                                 curl_off_t ulnow) {
    auto* transfer = static_cast<Transfer*>(user);
    const curl_off_t moved = dlnow + ulnow;
    if (moved != transfer->lastProgress) {
        transfer->lastProgress = moved;
        transfer->lastActivity = Clock::now();
    }
    return 0;
}

bool HttpClient::Impl::buildHeaders(Transfer& transfer, std::string& refusal) {
    curl_slist* list = nullptr;
    auto add = [&list](std::string& line) {
        curl_slist* grown = curl_slist_append(list, line.c_str());
        wipe(line);
        if (grown == nullptr) {
            return false;
        }
        list = grown;
        return true;
    };
    bool ok = true;
    for (const Header& header : transfer.request.headers) {
        if (!validHeaderName(header.name) || !validHeaderValue(header.value)) {
            refusal = "a request header is not valid";
            ok = false;
            break;
        }
        std::string line = header.name + ": " + header.value;
        if (header.secret) {
            const std::string_view secret = header.secret->reveal();
            if (!validHeaderValue(secret)) {
                wipe(line);
                refusal = "a request header is not valid";
                ok = false;
                break;
            }
            line.append(secret);
        }
        if (!add(line)) {
            refusal = "out of memory";
            ok = false;
            break;
        }
    }
    if (ok) {
        // No "Expect: 100-continue" round trip, whatever the body size.
        std::string expect = "Expect:";
        ok = add(expect);
        if (!ok) {
            refusal = "out of memory";
        }
    }
    if (!ok) {
        freeHeaderList(list);
        return false;
    }
    transfer.headerList = list;
    return true;
}

// Creates the easy handle for the current attempt and adds it to the multi handle. On failure the
// handle is released and `refusal` (a rule was broken) or `failure` (libcurl said no) is set.
bool HttpClient::Impl::beginAttempt(Transfer& transfer, std::string& refusal, CURLcode& failure) {
    failure = CURLE_OK;
    transfer.errorBuffer.fill('\0');
    transfer.capture.reset();
    transfer.headDelivered = false;
    transfer.gotFirstByte = false;
    transfer.bytesIn = 0;
    transfer.lastProgress = -1;
    transfer.lastActivity = Clock::now();
    if (!buildHeaders(transfer, refusal)) {
        return false;
    }
    CURL* easy = curl_easy_init();
    if (easy == nullptr) {
        freeHeaderList(transfer.headerList);
        failure = CURLE_FAILED_INIT;
        return false;
    }
    transfer.easy = easy;
    const TransferPlan& plan = transfer.plan;
    const Request& request = transfer.request;
    CURLcode rc = CURLE_OK;
    auto check = [&rc](CURLcode code) {
        if (rc == CURLE_OK) {
            rc = code;
        }
    };
    check(curl_easy_setopt(easy, CURLOPT_URL, request.url.c_str()));
    check(curl_easy_setopt(easy, CURLOPT_PRIVATE, &transfer));
    check(curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, transfer.errorBuffer.data()));
    check(curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L));
    // Only the planned protocol, and never a redirect: a redirect target is another host that
    // could receive the key, so a 3xx is reported as an error and not followed.
    check(curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, plan.protocols.c_str()));
    check(curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, plan.protocols.c_str()));
    check(curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L));
    check(curl_easy_setopt(easy, CURLOPT_MAXREDIRS, 0L));
    if (plan.disableProxy) {
        // A proxy from the environment must never see a request meant for loopback.
        check(curl_easy_setopt(easy, CURLOPT_PROXY, ""));
        check(curl_easy_setopt(easy, CURLOPT_NOPROXY, "*"));
    }
    check(curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, plan.connectTimeoutMs));
    // The stall guard is the client's own (checkStalls): libcurl's low-speed limit averages over a
    // window and fires several seconds late. The progress callback only feeds it.
    check(curl_easy_setopt(easy, CURLOPT_NOPROGRESS, 0L));
    check(curl_easy_setopt(easy, CURLOPT_XFERINFOFUNCTION, &HttpClient::Impl::onProgress));
    check(curl_easy_setopt(easy, CURLOPT_XFERINFODATA, &transfer));
    check(curl_easy_setopt(easy, CURLOPT_TCP_KEEPALIVE, 1L));
    check(curl_easy_setopt(easy, CURLOPT_TCP_KEEPIDLE, kKeepAliveIdleSeconds));
    check(curl_easy_setopt(easy, CURLOPT_TCP_KEEPINTVL, kKeepAliveIntervalSeconds));
    // No content encoding: a compressed event stream can be held back by the compressor until a
    // block fills, which would delay tokens; an answer is small enough that compression saves
    // nothing worth that.
    check(curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, "identity"));
    check(curl_easy_setopt(easy, CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L));
    if (!request.body.empty()) {
        check(curl_easy_setopt(easy, CURLOPT_POST, 1L));
        check(curl_easy_setopt(easy, CURLOPT_POSTFIELDS, request.body.data()));
        check(curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE_LARGE,
                               static_cast<curl_off_t>(request.body.size())));
    }
    check(curl_easy_setopt(easy, CURLOPT_HTTPHEADER, transfer.headerList));
    check(curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &HttpClient::Impl::onWrite));
    check(curl_easy_setopt(easy, CURLOPT_WRITEDATA, &transfer));
    check(curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &HttpClient::Impl::onHeader));
    check(curl_easy_setopt(easy, CURLOPT_HEADERDATA, &transfer));
    check(options.tls.apply(easy, transfer.attempt));
    if (rc == CURLE_OK) {
        const CURLMcode added = curl_multi_add_handle(multi, easy);
        if (added != CURLM_OK) {
            rc = CURLE_FAILED_INIT;
        }
    }
    if (rc != CURLE_OK) {
        curl_easy_cleanup(easy);
        transfer.easy = nullptr;
        freeHeaderList(transfer.headerList);
        failure = rc;
        return false;
    }
    return true;
}

void HttpClient::Impl::complete(std::unique_ptr<Transfer> transfer, Result result) {
    // The token's waker stays: it is the same for every request of this client, so that clearing
    // it here would leave a second request that shares the token without a waker. After the client
    // is gone it does nothing (WakeHandle::detach).
    result.timings.totalMs = millisSince(transfer->start, Clock::now());
    const auto onComplete = std::move(transfer->request.callbacks.onComplete);
    if (onComplete) {
        try {
            onComplete(result);
        } catch (...) {
            // A completion handler must not throw; there is nobody left to tell.
        }
    }
}

void HttpClient::Impl::startTransfer(std::unique_ptr<Transfer> owned) {
    Transfer& transfer = *owned;
    transfer.start = Clock::now();
    Result result;
    if (selfOwned) {
        // The client was destroyed by an earlier callback in this very batch.
        result.end = End::Shutdown;
        complete(std::move(owned), std::move(result));
        return;
    }
    if (transfer.request.cancel.cancelled()) {
        result.end = End::Cancelled;
        complete(std::move(owned), std::move(result));
        return;
    }
    bool carriesSecret = false;
    for (const Header& header : transfer.request.headers) {
        carriesSecret = carriesSecret || header.secret != nullptr;
    }
    transfer.plan =
        planTransfer(transfer.request.url, transfer.request.endpoint, carriesSecret, options);
    if (!transfer.plan.refusal.empty()) {
        result.end = End::Refused;
        result.refusal = transfer.plan.refusal;
        complete(std::move(owned), std::move(result));
        return;
    }
    const auto first = options.tls.firstAttempt(preferProbed.load());
    if (!first.has_value()) {
        result.end = End::Refused;
        result.refusal = "no system CA bundle was found";
        complete(std::move(owned), std::move(result));
        return;
    }
    transfer.attempt = *first;
    std::string refusal;
    CURLcode failure = CURLE_OK;
    if (!beginAttempt(transfer, refusal, failure)) {
        if (!refusal.empty()) {
            result.end = End::Refused;
            result.refusal = refusal;
        } else {
            result.curlCode = static_cast<int>(failure);
        }
        complete(std::move(owned), std::move(result));
        return;
    }
    active.push_back(std::move(owned));
}

void HttpClient::Impl::startItem(Item item) {
    if (item.task) {
        try {
            item.task();
        } catch (...) {
            // A posted task must not throw.
        }
        return;
    }
    startTransfer(std::move(item.transfer));
}

void HttpClient::Impl::scanCancelled() {
    // finish() removes entries, so walk a snapshot of the pointers.
    std::vector<Transfer*> cancelled;
    for (const auto& transfer : active) {
        if (transfer->request.cancel.cancelled()) {
            cancelled.push_back(transfer.get());
        }
    }
    for (Transfer* transfer : cancelled) {
        finish(transfer, End::Cancelled, CURLE_OK);
    }
}

// Drops every transfer that has been silent for its stall time and returns how long the next poll
// may block: up to the nearest stall deadline, never more than kBusyPollMs.
int HttpClient::Impl::checkStalls() {
    const auto now = Clock::now();
    std::vector<Transfer*> stalled;
    long long waitMs = active.empty() ? kIdlePollMs : kBusyPollMs;
    for (const auto& transfer : active) {
        const long long idle = millisSince(transfer->lastActivity, now);
        if (idle >= transfer->plan.stallMs) {
            stalled.push_back(transfer.get());
        } else {
            waitMs = std::min(waitMs, transfer->plan.stallMs - idle);
        }
    }
    for (Transfer* transfer : stalled) {
        transfer->stalled = true;
        finish(transfer, End::Finished, CURLE_OPERATION_TIMEDOUT);
    }
    return static_cast<int>(std::max<long long>(waitMs, 1));
}

void HttpClient::Impl::drainMessages() {
    std::vector<std::pair<Transfer*, CURLcode>> done;
    int remaining = 0;
    while (CURLMsg* message = curl_multi_info_read(multi, &remaining)) {
        if (message->msg != CURLMSG_DONE) {
            continue;
        }
        Transfer* transfer = nullptr;
        char* privateData = nullptr;
        curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &privateData);
        transfer = reinterpret_cast<Transfer*>(privateData);
        done.emplace_back(transfer, message->data.result);
    }
    for (const auto& [transfer, code] : done) {
        finish(transfer, End::Finished, code);
    }
}

// Ends one active transfer: stops it, closes its connection, then reports. A certificate-trust
// failure before any response byte is retried once with the located CA bundle (TlsPolicy).
void HttpClient::Impl::finish(Transfer* raw, End end, CURLcode code) {
    const auto found = std::find_if(active.begin(), active.end(),
                                    [raw](const auto& entry) { return entry.get() == raw; });
    if (found == active.end()) {
        return;
    }
    Result result;
    result.end = end;
    result.curlCode = static_cast<int>(code);
    if (end == End::Finished && code == CURLE_WRITE_ERROR) {
        if (raw->cancelSeen || raw->request.cancel.cancelled()) {
            result.end = End::Cancelled;
            result.curlCode = 0;
        } else if (raw->consumerAbort) {
            result.abortedByConsumer = true;
        }
    }

    CURL* easy = raw->easy;
    curl_multi_remove_handle(multi, easy);

    if (result.end == End::Finished && !raw->headDelivered && raw->bytesIn == 0) {
        if (const auto next = options.tls.retryAfter(static_cast<int>(code), raw->attempt)) {
            curl_easy_cleanup(easy);
            raw->easy = nullptr;
            freeHeaderList(raw->headerList);
            raw->attempt = *next;
            raw->tlsRetried = true;
            std::string refusal;
            CURLcode failure = CURLE_OK;
            if (beginAttempt(*raw, refusal, failure)) {
                return;  // still active, now on the second attempt
            }
            result.curlCode = static_cast<int>(failure != CURLE_OK ? failure : CURLE_FAILED_INIT);
            if (!refusal.empty()) {
                result.end = End::Refused;
                result.refusal = refusal;
            }
            easy = nullptr;
        }
    }

    if (easy != nullptr) {
        long status = 0;
        curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
        curl_off_t connectMicros = 0;
        curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &connectMicros);
        curl_off_t sent = 0;
        curl_easy_getinfo(easy, CURLINFO_SIZE_UPLOAD_T, &sent);
        result.httpStatus =
            raw->capture.status() != 0 ? raw->capture.status() : static_cast<int>(status);
        result.bytesOut = sent > 0 ? static_cast<std::uint64_t>(sent) : std::uint64_t{0};
        if (connectMicros > 0 || raw->gotFirstByte) {
            result.timings.connectMs = static_cast<std::int64_t>(connectMicros / 1000);
        }
        if (raw->errorBuffer[0] != '\0') {
            raw->errorBuffer.back() = '\0';
            result.curlMessage = redact(providers::printableExcerpt(raw->errorBuffer.data(), 200));
        }
        // Closing the handle closes the socket of a transfer that did not finish.
        curl_easy_cleanup(easy);
        raw->easy = nullptr;
        freeHeaderList(raw->headerList);
    }
    result.requestId = raw->capture.requestId();
    result.retryAfter = raw->capture.retryAfter();
    result.stalled = raw->stalled && result.end == End::Finished;
    result.bytesIn = raw->bytesIn;
    result.tlsRetried = raw->tlsRetried;
    if (raw->gotFirstByte) {
        result.timings.firstByteMs = millisSince(raw->start, raw->firstByte);
    }
    if (raw->tlsRetried && result.end == End::Finished &&
        (result.curlCode == 0 || raw->headDelivered)) {
        preferProbed.store(true);
    }

    std::unique_ptr<Transfer> owned = std::move(*found);
    active.erase(found);
    complete(std::move(owned), std::move(result));
}

void HttpClient::Impl::shutdownAll() {
    while (!active.empty()) {
        finish(active.back().get(), End::Shutdown, CURLE_OK);
    }
}

void HttpClient::Impl::run() {
    for (;;) {
        std::deque<Item> items;
        bool stop = false;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            items.swap(pending);
            stop = stopping;
        }
        if (stop) {
            for (Item& item : items) {
                if (item.transfer) {
                    item.transfer->start = Clock::now();
                    Result result;
                    result.end = End::Shutdown;
                    complete(std::move(item.transfer), std::move(result));
                }
            }
            shutdownAll();
            if (selfOwned) {
                delete this;  // the destructor of HttpClient gave this object to the net thread
            }
            return;
        }
        for (Item& item : items) {
            startItem(std::move(item));
        }
        scanCancelled();
        int running = 0;
        curl_multi_perform(multi, &running);
        drainMessages();
        const int timeout = checkStalls();
        {
            // A callback may have submitted a request or destroyed the client: go round at once.
            const std::lock_guard<std::mutex> lock(mutex);
            if (stopping || !pending.empty()) {
                continue;
            }
        }
        curl_multi_poll(multi, nullptr, 0, timeout, nullptr);
    }
}

// --- the public class ----------------------------------------------------------------------------

HttpClient::HttpClient(ClientOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {
    ensureCurlInitialised();
    impl_->multi = curl_multi_init();
    if (impl_->multi == nullptr) {
        return;
    }
    impl_->wake->multi = impl_->multi;
    try {
        impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    } catch (...) {
        impl_->wake->detach();
        curl_multi_cleanup(impl_->multi);
        impl_->multi = nullptr;
    }
}

HttpClient::~HttpClient() {
    if (!impl_) {
        return;
    }
    if (impl_->thread.joinable()) {
        if (impl_->thread.get_id() == std::this_thread::get_id()) {
            // A callback on the net thread dropped the last owner. The thread cannot join itself:
            // it finishes the batch it is in, ends what is in flight, frees the state and exits.
            {
                const std::lock_guard<std::mutex> lock(impl_->mutex);
                impl_->stopping = true;
            }
            impl_->selfOwned = true;
            impl_->thread.detach();
            static_cast<void>(impl_.release());
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->stopping = true;
        }
        impl_->wake->wake();
        impl_->thread.join();
    }
    // ~Impl releases the wake handle's pointer and the multi handle.
}

bool HttpClient::ok() const noexcept {
    return impl_->thread.joinable();
}

void HttpClient::submit(Request request) {
    auto transfer = std::make_unique<Transfer>();
    transfer->request = std::move(request);
    transfer->start = Clock::now();
    bool accepted = false;
    if (ok()) {
        // Before the request is visible to the net thread, so that no cancellation is missed: if
        // the token is cancelled already the waker runs now, which only wakes an idle loop.
        transfer->request.cancel.setWaker([wake = impl_->wake] { wake->wake(); });
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->stopping) {
            Item item;
            item.transfer = std::move(transfer);
            impl_->pending.push_back(std::move(item));
            accepted = true;
        }
    }
    if (accepted) {
        impl_->wake->wake();
        return;
    }
    // The client cannot run it (no net thread, or shutting down): report on this thread.
    Result result;
    if (ok()) {
        result.end = End::Shutdown;
    } else {
        result.curlCode = static_cast<int>(CURLE_FAILED_INIT);
    }
    // `transfer` was moved from only in the branch that returned above (the request is queued).
    // NOLINTNEXTLINE(bugprone-use-after-move)
    const auto onComplete = std::move(transfer->request.callbacks.onComplete);
    if (onComplete) {
        try {
            onComplete(result);
        } catch (...) {
        }
    }
}

void HttpClient::post(std::function<void()> task) {
    if (!ok() || !task) {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->mutex);
        if (impl_->stopping) {
            return;
        }
        Item item;
        item.task = std::move(task);
        impl_->pending.push_back(std::move(item));
    }
    impl_->wake->wake();
}

}  // namespace curia::http
