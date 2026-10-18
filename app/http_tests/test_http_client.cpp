// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "http/http_client.h"
#include "silent_server.h"

using namespace std::chrono_literals;
using curia::SecretString;
using curia::http::ClientOptions;
using curia::http::ConnectionEnd;
using curia::http::End;
using curia::http::Header;
using curia::http::HttpClient;
using curia::http::planTransfer;
using curia::http::Request;
using curia::http::Result;
using curia::net::Endpoint;
using curia::providers::CancelToken;

namespace {

Endpoint realEndpoint() {
    return Endpoint{"https://api.anthropic.com/v1/messages", false, false, "api.anthropic.com"};
}

Endpoint testEndpoint(int port) {
    const std::string host = "127.0.0.1";
    return Endpoint{"http://" + host + ":" + std::to_string(port) + "/v1/messages", true, true,
                    host};
}

#if defined(CURIA_ENABLE_DEV_ENV)

std::shared_ptr<const SecretString> fakeKey() {
    return std::make_shared<const SecretString>(std::string("sk-ant-") + "FAKE" + "-unit-key-0123");
}

// A request whose completion lands in `done`.
Request makeRequest(const Endpoint& endpoint, std::promise<Result>& done) {
    Request request;
    request.url = endpoint.url;
    request.endpoint = endpoint;
    request.headers.push_back(Header::plain("content-type", "application/json"));
    request.body = "{}";
    request.callbacks.onComplete = [&done](const Result& result) { done.set_value(result); };
    return request;
}

bool ready(std::future<Result>& future, std::chrono::milliseconds timeout) {
    return future.wait_for(timeout) == std::future_status::ready;
}

#endif  // CURIA_ENABLE_DEV_ENV

}  // namespace

// --- planTransfer: the rules, without libcurl
// --------------------------------------------------------

TEST_CASE("http-client: a real endpoint is https only, proxies stay allowed") {
    const auto plan =
        planTransfer("https://api.anthropic.com/v1/messages", realEndpoint(), true, {});
    CHECK(plan.refusal.empty());
    CHECK(plan.protocols == "https");
    CHECK_FALSE(plan.disableProxy);
}

TEST_CASE("http-client: plain http is refused for a real endpoint") {
    const Endpoint real = realEndpoint();
    CHECK_FALSE(
        planTransfer("http://api.anthropic.com/v1/messages", real, false, {}).refusal.empty());
    CHECK_FALSE(
        planTransfer("ftp://api.anthropic.com/v1/messages", real, false, {}).refusal.empty());
    CHECK_FALSE(planTransfer("api.anthropic.com/v1/messages", real, false, {}).refusal.empty());
}

#if defined(CURIA_ENABLE_DEV_ENV)

TEST_CASE("http-client: a test endpoint is loopback http, no proxy") {
    const Endpoint test = testEndpoint(4321);
    const auto plan = planTransfer(test.url, test, true, {});
    CHECK(plan.refusal.empty());
    CHECK(plan.protocols == "http");
    CHECK(plan.disableProxy);
    // ... and https to the same place is not what the endpoint says.
    CHECK_FALSE(
        planTransfer("https://127.0.0.1:4321/v1/messages", test, false, {}).refusal.empty());
}

#else

TEST_CASE("http-client: a release build refuses even a well-formed test endpoint") {
    const Endpoint test = testEndpoint(4321);
    CHECK_FALSE(planTransfer(test.url, test, true, {}).refusal.empty());
}

#endif

// The Endpoint is a plain aggregate that any code can fill in; the client must not take its flags
// on trust (the key would go in clear text to whatever host it names).
TEST_CASE("http-client: a forged endpoint does not get plain http, no proxy or the key") {
    const std::vector<Endpoint> forged = {
        {"http://evil.example.com:80/v1/messages", true, true, "evil.example.com"},
        {"http://192.0.2.1:80/v1/messages", true, true, "192.0.2.1"},
        {"http://localhost:80/v1/messages", true, true, "localhost"},
        {"http://128.0.0.1:80/v1/messages", true, true, "128.0.0.1"},
        {"http://127.0.0.1:80@evil.example.com/v1/messages", true, true, "127.0.0.1"},
        {"https://127.0.0.1:80/v1/messages", true, true, "127.0.0.1"},
        {"https://evil.example.com/v1/messages", false, false, "evil.example.com"},
        {"https://api.anthropic.com.evil.example/v1/messages", false, false, "api.anthropic.com"},
        {"https://api.anthropic.com/v1/other", false, false, "api.anthropic.com"},
        {"http://127.0.0.1:80/v1/messages", false, false, "127.0.0.1"},
        {"http://api.anthropic.com/v1/messages", true, true, "api.anthropic.com"},
        {"https://api.anthropic.com/v1/messages", true, true, "api.anthropic.com"},
    };
    for (const Endpoint& endpoint : forged) {
        for (const bool secret : {false, true}) {
            CAPTURE(endpoint.url);
            CAPTURE(secret);
            CHECK_FALSE(planTransfer(endpoint.url, endpoint, secret, {}).refusal.empty());
        }
    }
}

TEST_CASE("http-client: another origin, userinfo, odd bytes and an empty address are refused") {
    const Endpoint real = realEndpoint();
    for (const char* url :
         {"https://evil.example/v1/messages", "https://api.anthropic.com:444/v1/messages",
          "https://api.anthropic.com@evil.example/v1/messages",
          "https://user@api.anthropic.com/v1/messages",
          "https://api.anthropic.com.evil.example/v1/messages", "https://api.anthropic.com",
          "https://api.anthropic.com/v1/messages\r\nX: y", "https://api.anthropic.com/v1/mess ages",
          "https://api.anthropic.com/v1\\messages", ""}) {
        CAPTURE(url);
        CHECK_FALSE(planTransfer(url, real, false, {}).refusal.empty());
    }
    Endpoint none;
    CHECK_FALSE(
        planTransfer("https://api.anthropic.com/v1/messages", none, false, {}).refusal.empty());
}

TEST_CASE("http-client: the key goes only to the endpoint's own URL") {
    const Endpoint real = realEndpoint();
    CHECK(planTransfer(real.url, real, true, {}).refusal.empty());
    // Same origin, another path (say, a future models call): fine without a key, refused with one.
    const std::string other = "https://api.anthropic.com/v1/models";
    CHECK(planTransfer(other, real, false, {}).refusal.empty());
    CHECK_FALSE(planTransfer(other, real, true, {}).refusal.empty());
}

TEST_CASE("http-client: the timeouts come from the options") {
    const Endpoint real = realEndpoint();
    ClientOptions defaults;
    auto plan = planTransfer(real.url, real, false, defaults);
    CHECK(plan.connectTimeoutMs == 10000);
    CHECK(plan.stallMs == 45000);
    CHECK(plan.stallSeconds == 45);

    ClientOptions custom;
    custom.connectTimeout = 2500ms;
    custom.stallTimeout = 1ms;
    plan = planTransfer(real.url, real, false, custom);
    CHECK(plan.connectTimeoutMs == 2500);
    CHECK(plan.stallMs == 1);       // exact, at least one millisecond
    CHECK(plan.stallSeconds == 1);  // for the message: whole seconds, at least one

    custom.stallTimeout = 1500ms;
    plan = planTransfer(real.url, real, false, custom);
    CHECK(plan.stallMs == 1500);
    CHECK(plan.stallSeconds == 2);  // rounded up

    custom.stallTimeout = 0ms;
    custom.connectTimeout = 0ms;
    plan = planTransfer(real.url, real, false, custom);
    CHECK(plan.stallMs == 1);
    CHECK(plan.connectTimeoutMs == 1);

    // Out of range values are clamped, also when the request itself is refused.
    custom.stallTimeout = std::chrono::hours(100);
    custom.connectTimeout = std::chrono::hours(100);
    plan = planTransfer("", real, false, custom);
    CHECK_FALSE(plan.refusal.empty());
    CHECK(plan.stallMs == 3600 * 1000);
    CHECK(plan.connectTimeoutMs == 10 * 60 * 1000);
}

TEST_CASE("http-client: the connection and HTTP version words of the app log") {
    using curia::http::httpVersionWord;
    using curia::http::toString;
    CHECK(std::string(toString(ConnectionEnd::None)) == "none");
    CHECK(std::string(toString(ConnectionEnd::Closed)) == "closed");
    CHECK(std::string(toString(ConnectionEnd::StreamReset)) == "stream_reset");
    CHECK(std::string(toString(ConnectionEnd::Released)) == "released");
    CHECK(std::string(httpVersionWord(0)) == "none");
    CHECK(std::string(httpVersionWord(1)) == "1.0");  // CURL_HTTP_VERSION_1_0
    CHECK(std::string(httpVersionWord(2)) == "1.1");
    CHECK(std::string(httpVersionWord(3)) == "2");
    CHECK(std::string(httpVersionWord(30)) == "3");
    CHECK(std::string(httpVersionWord(99)) == "none");
}

// --- the client: lifecycle without a server
// ------------------------------------------------------------

#if defined(CURIA_ENABLE_DEV_ENV)

TEST_CASE("http-client: a request cancelled before it starts sends nothing") {
    HttpClient client;
    REQUIRE(client.ok());
    std::promise<Result> done;
    auto future = done.get_future();
    // Nothing listens on this port, and the request must not even try.
    Request request = makeRequest(testEndpoint(1), done);
    bool dataSeen = false;
    request.callbacks.onData = [&dataSeen](std::string_view) {
        dataSeen = true;
        return true;
    };
    request.cancel.cancel();
    client.submit(std::move(request));
    REQUIRE(ready(future, 5s));
    const Result result = future.get();
    CHECK((result.end == End::Cancelled));
    CHECK(result.curlCode == 0);
    CHECK(result.bytesOut == 0);
    CHECK(result.timings.connectMs == -1);
    CHECK((result.connection == ConnectionEnd::None));
    CHECK(result.newConnections == 0);
    CHECK_FALSE(dataSeen);
}

TEST_CASE("http-client: a refused request is completed, not dropped") {
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    Endpoint endpoint = testEndpoint(1);
    Request request = makeRequest(endpoint, done);
    request.url = "http://127.0.0.1:2/v1/messages";  // not the endpoint's origin
    client.submit(std::move(request));
    REQUIRE(ready(future, 5s));
    const Result result = future.get();
    CHECK((result.end == End::Refused));
    CHECK_FALSE(result.refusal.empty());
}

TEST_CASE("http-client: a key never goes to a URL other than the endpoint's") {
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    const Endpoint endpoint = testEndpoint(1);
    Request request = makeRequest(endpoint, done);
    request.url = "http://127.0.0.1:1/v1/other";  // same origin, not the endpoint URL
    request.headers.push_back(Header::withSecret("authorization", "Bearer ", fakeKey()));
    client.submit(std::move(request));
    REQUIRE(ready(future, 5s));
    const Result result = future.get();
    CHECK((result.end == End::Refused));
    CHECK(result.bytesOut == 0);
}

TEST_CASE("http-client: bad header names and values are refused") {
    HttpClient client;
    for (const std::string& bad : {std::string("x-test\r\nInjected: 1"), std::string("")}) {
        std::promise<Result> done;
        auto future = done.get_future();
        Request request = makeRequest(testEndpoint(1), done);
        request.headers.push_back(Header::plain(bad, "v"));
        client.submit(std::move(request));
        REQUIRE(ready(future, 5s));
        CHECK((future.get().end == End::Refused));
    }
    std::promise<Result> done;
    auto future = done.get_future();
    Request request = makeRequest(testEndpoint(1), done);
    request.headers.push_back(Header::plain("x-test", "a\r\nInjected: 1"));
    client.submit(std::move(request));
    REQUIRE(ready(future, 5s));
    CHECK((future.get().end == End::Refused));
}

TEST_CASE("http-client: a refused connection is a finished transfer with a curl code") {
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    client.submit(makeRequest(testEndpoint(1), done));
    REQUIRE(ready(future, 10s));
    const Result result = future.get();
    CHECK((result.end == End::Finished));
    CHECK(result.curlCode != 0);
    CHECK(result.httpStatus == 0);
    CHECK(result.bytesIn == 0);
    // libcurl ended it by itself: what became of the socket is libcurl's call.
    CHECK((result.connection == ConnectionEnd::Released));
    CHECK(result.httpVersion == 0);
    CHECK(std::string(curia::http::httpVersionWord(result.httpVersion)) == "none");
}

TEST_CASE("http-client: post runs a task on the net thread, in order") {
    HttpClient client;
    std::promise<std::thread::id> ran;
    client.post([&ran] { ran.set_value(std::this_thread::get_id()); });
    auto future = ran.get_future();
    REQUIRE(future.wait_for(5s) == std::future_status::ready);
    CHECK(future.get() != std::this_thread::get_id());
}

#if !defined(_WIN32)

TEST_CASE(
    "http-client: destroying the client mid-request ends it with Shutdown and closes the socket") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    std::promise<Result> done;
    auto future = done.get_future();
    std::atomic<int> completions{0};
    {
        HttpClient client;
        Request request = makeRequest(testEndpoint(server.port()), done);
        request.callbacks.onComplete = [&done, &completions](const Result& result) {
            ++completions;
            done.set_value(result);
        };
        client.submit(std::move(request));
        REQUIRE(server.waitAccepted(5s));
        // The client is destroyed here, with the request in flight.
    }
    // The destructor joined the net thread, so the completion has already run.
    REQUIRE(ready(future, 0ms));
    const Result result = future.get();
    CHECK((result.end == End::Shutdown));
    CHECK((result.connection == ConnectionEnd::Closed));
    CHECK(completions.load() == 1);
    CHECK(server.waitPeerClosed(5s));
}

TEST_CASE("http-client: cancelling a request in flight closes the socket") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    Request request = makeRequest(testEndpoint(server.port()), done);
    CancelToken token = request.cancel;
    client.submit(std::move(request));
    REQUIRE(server.waitAccepted(5s));
    token.cancel();
    REQUIRE(ready(future, 5s));
    const Result result = future.get();
    CHECK((result.end == End::Cancelled));
    // What the app log's net_done says (conn=closed), and what the server sees.
    CHECK((result.connection == ConnectionEnd::Closed));
    CHECK(std::string(curia::http::toString(result.connection)) == "closed");
    CHECK(result.newConnections == 1);
    CHECK(server.waitPeerClosed(5s));
}

TEST_CASE("http-client: the stall guard drops a connection that stays silent, on time") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    ClientOptions options;
    options.stallTimeout = 1000ms;
    HttpClient client(options);
    std::promise<Result> done;
    auto future = done.get_future();
    const auto started = std::chrono::steady_clock::now();
    client.submit(makeRequest(testEndpoint(server.port()), done));
    REQUIRE(ready(future, 15s));
    const Result result = future.get();
    const auto took = std::chrono::steady_clock::now() - started;
    CHECK((result.end == End::Finished));
    CHECK(result.curlCode == 28);  // CURLE_OPERATION_TIMEDOUT
    CHECK(result.stalled);
    CHECK((result.connection == ConnectionEnd::Closed));
    // The client's own clock: the configured second, not libcurl's averaging window on top.
    CHECK(took >= 900ms);
    CHECK(took < 2500ms);
    CHECK(server.waitPeerClosed(5s));
}

TEST_CASE("http-client: cancel from another thread wakes an idle net thread at once") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    Request request = makeRequest(testEndpoint(server.port()), done);
    CancelToken token = request.cancel;
    client.submit(std::move(request));
    REQUIRE(server.waitAccepted(5s));
    // The net thread is now blocked in curl_multi_poll with nothing to do; without the token's
    // waker it would notice the cancel only when its one-second poll timeout runs out.
    std::this_thread::sleep_for(100ms);
    const auto cancelledAt = std::chrono::steady_clock::now();
    token.cancel();
    REQUIRE(ready(future, 5s));
    const auto latency = std::chrono::steady_clock::now() - cancelledAt;
    CHECK((future.get().end == End::Cancelled));
    CHECK(latency < 400ms);
    CHECK(server.waitPeerClosed(5s));
}

TEST_CASE("http-client: one token shared by two requests still wakes the net thread") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    HttpClient client;
    CancelToken token;

    std::promise<Result> silentDone;
    auto silentFuture = silentDone.get_future();
    Request silent = makeRequest(testEndpoint(server.port()), silentDone);
    silent.cancel = token;
    client.submit(std::move(silent));
    REQUIRE(server.waitAccepted(5s));

    // A second request with the same token ends at once (nothing listens on port 1); completing
    // it must not take the first request's waker away.
    std::promise<Result> refusedDone;
    auto refusedFuture = refusedDone.get_future();
    Request refused = makeRequest(testEndpoint(1), refusedDone);
    refused.cancel = token;
    client.submit(std::move(refused));
    REQUIRE(ready(refusedFuture, 10s));
    CHECK((refusedFuture.get().end == End::Finished));

    std::this_thread::sleep_for(100ms);
    const auto cancelledAt = std::chrono::steady_clock::now();
    token.cancel();
    REQUIRE(ready(silentFuture, 5s));
    const auto latency = std::chrono::steady_clock::now() - cancelledAt;
    CHECK((silentFuture.get().end == End::Cancelled));
    CHECK(latency < 400ms);
}

TEST_CASE("http-client: the last owner dropped by a callback on the net thread is safe") {
    curia::testing::SilentServer first;
    curia::testing::SilentServer second;
    REQUIRE(first.ok());
    REQUIRE(second.ok());

    auto client = std::make_shared<HttpClient>();
    std::promise<Result> secondDone;
    auto secondFuture = secondDone.get_future();
    client->submit(makeRequest(testEndpoint(second.port()), secondDone));

    std::promise<Result> firstDone;
    auto firstFuture = firstDone.get_future();
    Request request = makeRequest(testEndpoint(first.port()), firstDone);
    CancelToken token = request.cancel;
    // The only owner left after client.reset() lives in this callback, which runs on the net
    // thread: the client is destroyed there.
    request.callbacks.onComplete = [&firstDone, owner = client](const Result& result) {
        firstDone.set_value(result);
    };
    client->submit(std::move(request));
    REQUIRE(first.waitAccepted(5s));
    REQUIRE(second.waitAccepted(5s));
    client.reset();

    token.cancel();
    REQUIRE(ready(firstFuture, 5s));
    CHECK((firstFuture.get().end == End::Cancelled));
    // The request that was still in flight is ended by the client's shutdown, and its connection
    // is closed; the process did not abort on the way.
    REQUIRE(ready(secondFuture, 5s));
    CHECK((secondFuture.get().end == End::Shutdown));
    CHECK(second.waitPeerClosed(5s));
    CHECK(first.waitPeerClosed(5s));
}

TEST_CASE("http-client: a large body does not make the client send Expect: 100-continue") {
    curia::testing::SilentServer server;
    REQUIRE(server.ok());
    HttpClient client;
    std::promise<Result> done;
    auto future = done.get_future();
    Request request = makeRequest(testEndpoint(server.port()), done);
    request.body.assign(3 * 1024 * 1024, 'x');  // above any threshold libcurl has for Expect
    CancelToken token = request.cancel;
    client.submit(std::move(request));
    REQUIRE(server.waitAccepted(5s));
    std::string head = server.readRequestHead(5s);
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    CHECK(head.find("content-length: 3145728") != std::string::npos);
    CHECK(head.find("accept-encoding: identity") != std::string::npos);
    CHECK(head.find("expect:") == std::string::npos);
    token.cancel();
    REQUIRE(ready(future, 5s));
    CHECK((future.get().end == End::Cancelled));
}

#endif  // !_WIN32

#endif  // CURIA_ENABLE_DEV_ENV
