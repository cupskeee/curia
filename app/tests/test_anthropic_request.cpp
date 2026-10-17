// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <ostream>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_test_support.h"
#include "platform/env.h"
#include "providers/anthropic_request.h"

#ifndef CURIA_ANTHROPIC_FIXTURE_DIR
#error "CURIA_ANTHROPIC_FIXTURE_DIR must point at tests/fixtures/anthropic"
#endif

using curia::providers::buildAnthropicRequest;
using curia::providers::buildAnthropicRequestJson;
using curia::providers::BuiltRequest;
using curia::providers::ChatRequest;
using curia::providers::Effort;
using curia::providers::Message;
using curia::providers::Role;
using curia::providers::Thinking;
using nlohmann::json;

namespace {

ChatRequest basicRequest() {
    ChatRequest request;
    request.model = "mock-model-1";
    request.systemPersona = "Test persona text.";
    request.snapshotText = "Test snapshot text.";
    request.conversation = {
        {Role::User, "Test question one?"},
        {Role::Assistant, "Test answer one."},
        {Role::User, "Test question two?"},
    };
    request.maxTokens = 2048;
    return request;
}

json parsed(const std::string& body) {
    json value = json::parse(body, nullptr, false);
    REQUIRE_FALSE(value.is_discarded());
    return value;
}

bool hasCache(const json& block) {
    return block.contains("cache_control");
}

// Every object key anywhere in the value (an explicit stack: the code base avoids recursion).
void collectKeys(const json& root, std::set<std::string>& keys) {
    std::vector<const json*> pending = {&root};
    while (!pending.empty()) {
        const json* value = pending.back();
        pending.pop_back();
        if (value->is_object()) {
            for (const auto& [key, member] : value->items()) {
                keys.insert(key);
                pending.push_back(&member);
            }
        } else if (value->is_array()) {
            for (const json& element : *value) {
                pending.push_back(&element);
            }
        }
    }
}

std::string goldenPath(const std::string& name) {
    return std::string(CURIA_ANTHROPIC_FIXTURE_DIR) + "/" + name + ".json";
}

// The body must equal the committed file byte for byte (the file ends in one newline).
// CURIA_UPDATE_GOLDEN=1 rewrites the file; review the diff against the spec by hand.
void checkGolden(const std::string& name, const std::string& body) {
    CAPTURE(name);
    if (!curia::platform::getEnv("CURIA_UPDATE_GOLDEN").empty()) {
        REQUIRE(curia::bridge::test::writeFile(goldenPath(name), body + "\n"));
        return;
    }
    bool ok = false;
    const std::string golden = curia::bridge::test::readFile(goldenPath(name), ok);
    REQUIRE_MESSAGE(ok, "missing golden file");
    CHECK((golden == body + "\n"));
}

}  // namespace

TEST_CASE("request: golden bodies") {
    checkGolden("request_basic", buildAnthropicRequestJson(basicRequest()));

    ChatRequest noCache = basicRequest();
    noCache.cache.cacheSystem = false;
    noCache.cache.cacheSnapshot = false;
    checkGolden("request_no_cache", buildAnthropicRequestJson(noCache));

    ChatRequest longTtl = basicRequest();
    longTtl.cache.longTtl = true;
    checkGolden("request_ttl_1h", buildAnthropicRequestJson(longTtl));

    ChatRequest bare;
    bare.model = "mock-model-1";
    bare.conversation = {{Role::User, "Test question one?"}};
    checkGolden("request_no_system", buildAnthropicRequestJson(bare));
}

TEST_CASE("request: the basic shape") {
    const BuiltRequest built = buildAnthropicRequest(basicRequest());
    CHECK(built.problem.empty());
    const json body = parsed(built.body);
    REQUIRE(body.is_object());

    std::set<std::string> topLevel;
    for (const auto& [key, member] : body.items()) {
        topLevel.insert(key);
    }
    CHECK(
        (topLevel == std::set<std::string>{"max_tokens", "messages", "model", "stream", "system"}));
    CHECK((body["model"] == "mock-model-1"));
    CHECK((body["max_tokens"] == 2048));
    CHECK((body["stream"] == true));

    REQUIRE(body["system"].size() == 2);
    CHECK((body["system"][0]["type"] == "text"));
    CHECK((body["system"][0]["text"] == "Test persona text."));
    CHECK((body["system"][0]["cache_control"] == json{{"type", "ephemeral"}}));
    CHECK((body["system"][1]["text"] == "Test snapshot text."));
    CHECK((body["system"][1]["cache_control"] == json{{"type", "ephemeral"}}));

    REQUIRE(body["messages"].size() == 3);
    CHECK((body["messages"][0] == json{{"role", "user"}, {"content", "Test question one?"}}));
    CHECK((body["messages"][1] == json{{"role", "assistant"}, {"content", "Test answer one."}}));
    CHECK((body["messages"][2] == json{{"role", "user"}, {"content", "Test question two?"}}));
}

TEST_CASE("request: the bytes are compact and deterministic") {
    const std::string first = buildAnthropicRequestJson(basicRequest());
    CHECK((first == buildAnthropicRequestJson(basicRequest())));
    CHECK(first.find('\n') == std::string::npos);
    CHECK(first.find(": ") == std::string::npos);
    CHECK(first.find(", ") == std::string::npos);
    // Keys are sorted, whatever the order they were added in.
    CHECK(first.find("\"max_tokens\"") < first.find("\"messages\""));
    CHECK(first.find("\"messages\"") < first.find("\"model\""));
    CHECK(first.find("\"model\"") < first.find("\"stream\""));
    CHECK(first.find("\"stream\"") < first.find("\"system\""));
}

TEST_CASE("request: breakpoint combinations") {
    for (const bool system : {false, true}) {
        for (const bool snapshot : {false, true}) {
            for (const bool longTtl : {false, true}) {
                CAPTURE(system);
                CAPTURE(snapshot);
                CAPTURE(longTtl);
                ChatRequest request = basicRequest();
                request.cache.cacheSystem = system;
                request.cache.cacheSnapshot = snapshot;
                request.cache.longTtl = longTtl;
                const json body = parsed(buildAnthropicRequestJson(request));
                REQUIRE(body["system"].size() == 2);
                const json& persona = body["system"][0];
                const json& realm = body["system"][1];
                CHECK(hasCache(persona) == system);
                CHECK(hasCache(realm) == snapshot);
                if (system) {
                    CHECK((persona["cache_control"]["type"] == "ephemeral"));
                    CHECK(persona["cache_control"].contains("ttl") == longTtl);
                    if (longTtl) {
                        CHECK((persona["cache_control"]["ttl"] == "1h"));
                    }
                }
                if (snapshot) {
                    CHECK((realm["cache_control"]["type"] == "ephemeral"));
                    CHECK(realm["cache_control"].contains("ttl") == longTtl);
                    if (longTtl) {
                        CHECK((realm["cache_control"]["ttl"] == "1h"));
                    }
                }
                // The text is the same whatever the breakpoints.
                CHECK((persona["text"] == "Test persona text."));
                CHECK((realm["text"] == "Test snapshot text."));
            }
        }
    }
}

TEST_CASE("request: empty or blank system blocks are left out") {
    ChatRequest request = basicRequest();

    request.systemPersona.clear();
    json body = parsed(buildAnthropicRequestJson(request));
    REQUIRE(body["system"].size() == 1);
    CHECK((body["system"][0]["text"] == "Test snapshot text."));
    CHECK(hasCache(body["system"][0]));  // the snapshot's own flag still applies

    request = basicRequest();
    request.snapshotText.clear();
    body = parsed(buildAnthropicRequestJson(request));
    REQUIRE(body["system"].size() == 1);
    CHECK((body["system"][0]["text"] == "Test persona text."));

    request = basicRequest();
    request.systemPersona = " \n\t ";
    request.snapshotText = "\r\n";
    body = parsed(buildAnthropicRequestJson(request));
    CHECK_FALSE(body.contains("system"));

    request.systemPersona.clear();
    request.snapshotText.clear();
    body = parsed(buildAnthropicRequestJson(request));
    CHECK_FALSE(body.contains("system"));

    // Text with white space around it is kept as it is.
    request = basicRequest();
    request.systemPersona = "  keep me \n";
    body = parsed(buildAnthropicRequestJson(request));
    CHECK((body["system"][0]["text"] == "  keep me \n"));
}

TEST_CASE("request: model and max_tokens are passed through") {
    ChatRequest request = basicRequest();
    request.model = "mock-model-2";
    request.maxTokens = 1;
    json body = parsed(buildAnthropicRequestJson(request));
    CHECK((body["model"] == "mock-model-2"));
    CHECK((body["max_tokens"] == 1));

    request.model = "mock:some_scenario";
    request.maxTokens = 64000;
    body = parsed(buildAnthropicRequestJson(request));
    CHECK((body["model"] == "mock:some_scenario"));
    CHECK((body["max_tokens"] == 64000));

    // Escaped, not interpreted.
    request.model = "a\"b\\c";
    body = parsed(buildAnthropicRequestJson(request));
    CHECK((body["model"] == "a\"b\\c"));

    // The default of ChatRequest.
    ChatRequest defaults;
    defaults.conversation = {{Role::User, "Test question one?"}};
    body = parsed(buildAnthropicRequestJson(defaults));
    CHECK((body["max_tokens"] == 4096));
    CHECK(body["model"].get<std::string>().empty());
}

TEST_CASE("request: roles alternate, and the conversation rules") {
    const auto messagesOf = [](std::vector<Message> conversation) {
        ChatRequest request = basicRequest();
        request.conversation = std::move(conversation);
        return parsed(buildAnthropicRequest(request).body)["messages"];
    };
    const auto problemOf = [](std::vector<Message> conversation) {
        ChatRequest request = basicRequest();
        request.conversation = std::move(conversation);
        return buildAnthropicRequest(request).problem;
    };

    SUBCASE("consecutive messages of one role are merged with a blank line") {
        const json messages = messagesOf({{Role::User, "one"},
                                          {Role::User, "two"},
                                          {Role::Assistant, "three"},
                                          {Role::Assistant, "four"},
                                          {Role::Assistant, "five"},
                                          {Role::User, "six"}});
        REQUIRE(messages.size() == 3);
        CHECK((messages[0] == json{{"role", "user"}, {"content", "one\n\ntwo"}}));
        CHECK((messages[1] == json{{"role", "assistant"}, {"content", "three\n\nfour\n\nfive"}}));
        CHECK((messages[2] == json{{"role", "user"}, {"content", "six"}}));
    }
    SUBCASE("leading assistant messages are dropped") {
        const json messages =
            messagesOf({{Role::Assistant, "a"}, {Role::Assistant, "b"}, {Role::User, "c"}});
        REQUIRE(messages.size() == 1);
        CHECK((messages[0] == json{{"role", "user"}, {"content", "c"}}));
    }
    SUBCASE("system messages are skipped") {
        const json messages = messagesOf({{Role::System, "s1"},
                                          {Role::User, "u"},
                                          {Role::System, "s2"},
                                          {Role::Assistant, "a"},
                                          {Role::User, "v"}});
        REQUIRE(messages.size() == 3);
        CHECK((messages[0]["content"] == "u"));
        CHECK((messages[1]["content"] == "a"));
        CHECK((messages[2]["content"] == "v"));
    }
    SUBCASE("a system message between two user messages does not split them") {
        const json messages =
            messagesOf({{Role::User, "u1"}, {Role::System, "s"}, {Role::User, "u2"}});
        REQUIRE(messages.size() == 1);
        CHECK((messages[0]["content"] == "u1\n\nu2"));
    }
    SUBCASE("empty and blank messages are dropped, which can make neighbours merge") {
        const json messages = messagesOf({{Role::User, "u1"},
                                          {Role::Assistant, ""},
                                          {Role::User, "u2"},
                                          {Role::Assistant, " \n "},
                                          {Role::Assistant, "a"},
                                          {Role::User, ""}});
        REQUIRE(messages.size() == 2);
        CHECK((messages[0]["content"] == "u1\n\nu2"));
        CHECK((messages[1]["content"] == "a"));
    }
    SUBCASE("a dropped empty first user message makes the next assistant message leading") {
        const json messages =
            messagesOf({{Role::User, ""}, {Role::Assistant, "a"}, {Role::User, "u"}});
        REQUIRE(messages.size() == 1);
        CHECK((messages[0]["content"] == "u"));
    }
    SUBCASE("white space inside a message is kept") {
        const json messages = messagesOf({{Role::User, "  line one\nline two \n"}});
        REQUIRE(messages.size() == 1);
        CHECK((messages[0]["content"] == "  line one\nline two \n"));
    }
    SUBCASE("problems") {
        CHECK(problemOf({{Role::User, "u"}}).empty());
        CHECK(problemOf({{Role::User, "u"}, {Role::Assistant, "a"}, {Role::User, "v"}}).empty());
        CHECK_FALSE(problemOf({{Role::User, "u"}, {Role::Assistant, "a"}}).empty());
        CHECK_FALSE(problemOf({}).empty());
        CHECK_FALSE(problemOf({{Role::System, "s"}}).empty());
        CHECK_FALSE(problemOf({{Role::Assistant, "a"}}).empty());
        CHECK_FALSE(problemOf({{Role::User, " "}}).empty());
        CHECK_FALSE(
            problemOf({{Role::User, "u"}, {Role::System, "s"}, {Role::Assistant, "a"}}).empty());
        // A system message at the end does not matter.
        CHECK(problemOf({{Role::User, "u"}, {Role::System, "s"}}).empty());
    }
    SUBCASE("an unusable conversation still gives a parseable body") {
        ChatRequest request = basicRequest();
        request.conversation = {{Role::User, "u"}, {Role::Assistant, "a"}};
        const BuiltRequest built = buildAnthropicRequest(request);
        CHECK_FALSE(built.problem.empty());
        const json body = parsed(built.body);
        CHECK(body["messages"].size() == 2);
        request.conversation.clear();
        const BuiltRequest empty = buildAnthropicRequest(request);
        CHECK_FALSE(empty.problem.empty());
        CHECK(parsed(empty.body)["messages"].is_array());
    }
}

TEST_CASE("request: invalid UTF-8 is replaced, never thrown") {
    ChatRequest request = basicRequest();
    request.systemPersona = "persona \xFF end";
    request.snapshotText = "snap \xC3";  // a cut-off 2-byte sequence
    request.conversation = {{Role::User, "question"},
                            {Role::Assistant, "partial caf\xC3"},  // a cut-off answer
                            {Role::User, "continue \xE2\x82"}};
    const BuiltRequest built = buildAnthropicRequest(request);
    CHECK(built.problem.empty());
    const json body = parsed(built.body);  // it parses, so it is valid UTF-8
    const std::string replacement = "\xEF\xBF\xBD";
    CHECK((body["system"][0]["text"] == "persona " + replacement + " end"));
    CHECK((body["system"][1]["text"] == "snap " + replacement));
    CHECK((body["messages"][1]["content"] == "partial caf" + replacement));
    CHECK(
        (body["messages"][2]["content"].get<std::string>().find(replacement) != std::string::npos));

    // Valid text, however unusual, passes unchanged.
    request.systemPersona = "caf\xC3\xA9 \xE2\x82\xAC"
                            "5 \xF0\x9F\x98\x80 \"quoted\" back\\slash\ttab";
    const json valid = parsed(buildAnthropicRequestJson(request));
    CHECK((valid["system"][0]["text"] == "caf\xC3\xA9 \xE2\x82\xAC"
                                         "5 \xF0\x9F\x98\x80 \"quoted\" back\\slash\ttab"));
    // Non-ASCII is written as UTF-8, not as \u escapes.
    CHECK(buildAnthropicRequestJson(request).find("\xE2\x82\xAC") != std::string::npos);
}

TEST_CASE("request: invalid UTF-8 in the model name does not throw either") {
    ChatRequest request = basicRequest();
    request.model = "mock\xFF";
    const json body = parsed(buildAnthropicRequestJson(request));
    CHECK((body["model"] == "mock\xEF\xBF\xBD"));
}

TEST_CASE("request: by default no thinking, effort, sampling, tool or metadata keys, anywhere") {
    const std::set<std::string> forbidden = {"thinking",     "budget_tokens", "temperature",
                                             "top_p",        "top_k",         "tool_choice",
                                             "tools",        "metadata",      "stop_sequences",
                                             "service_tier", "output_config", "effort"};
    std::vector<ChatRequest> requests;
    requests.push_back(basicRequest());
    ChatRequest noSystem = basicRequest();
    noSystem.systemPersona.clear();
    noSystem.snapshotText.clear();
    requests.push_back(noSystem);
    ChatRequest thinking = basicRequest();
    thinking.showThinking = true;  // showing thinking is a display matter, not a request key
    thinking.cache.longTtl = true;
    requests.push_back(thinking);
    ChatRequest hostile = basicRequest();
    // Words that are keys elsewhere, inside text values: values, not keys.
    hostile.systemPersona = R"({"thinking":{"budget_tokens":1},"temperature":1})";
    hostile.conversation = {{Role::User, R"("top_p": 0.5, "tool_choice": {"type":"any"})"}};
    requests.push_back(hostile);

    for (const ChatRequest& request : requests) {
        const json body = parsed(buildAnthropicRequestJson(request));
        std::set<std::string> keys;
        collectKeys(body, keys);
        for (const std::string& key : forbidden) {
            CAPTURE(key);
            CHECK(keys.count(key) == 0);
        }
        // And the complete set of keys that may appear.
        const std::set<std::string> allowed = {"max_tokens", "messages", "model",  "stream",
                                               "system",     "type",     "text",   "cache_control",
                                               "ttl",        "role",     "content"};
        for (const std::string& key : keys) {
            CAPTURE(key);
            CHECK(allowed.count(key) == 1);
        }
    }
}

TEST_CASE("request: the effort and thinking settings, each alone and together") {
    const std::vector<std::pair<Effort, std::string>> efforts = {{Effort::Low, "low"},
                                                                 {Effort::Medium, "medium"},
                                                                 {Effort::High, "high"},
                                                                 {Effort::XHigh, "xhigh"},
                                                                 {Effort::Max, "max"}};
    for (const auto& entry : efforts) {
        const std::string& word = entry.second;
        CAPTURE(word);
        ChatRequest request = basicRequest();
        request.effort = entry.first;
        const json body = parsed(buildAnthropicRequestJson(request));
        CHECK((body["output_config"] == json{{"effort", word}}));
        CHECK_FALSE(body.contains("thinking"));
    }
    const std::vector<std::pair<Thinking, std::string>> modes = {
        {Thinking::Adaptive, "adaptive"},
        {Thinking::BetweenTools, "between_tools"},
        {Thinking::Disabled, "disabled"}};
    for (const auto& entry : modes) {
        const std::string& word = entry.second;
        CAPTURE(word);
        ChatRequest request = basicRequest();
        request.thinking = entry.first;
        const json body = parsed(buildAnthropicRequestJson(request));
        // Only the type: no budget, no display, nothing else next to it.
        CHECK((body["thinking"] == json{{"type", word}}));
        CHECK_FALSE(body.contains("output_config"));
    }

    ChatRequest both = basicRequest();
    both.effort = Effort::Low;
    both.thinking = Thinking::BetweenTools;
    const std::string bytes = buildAnthropicRequestJson(both);
    CHECK((bytes == buildAnthropicRequestJson(both)));  // deterministic
    // Sorted keys: max_tokens, messages, model, output_config, stream, system, thinking.
    CHECK(bytes.find("\"output_config\":{\"effort\":\"low\"}") != std::string::npos);
    CHECK(bytes.find("\"thinking\":{\"type\":\"between_tools\"}") != std::string::npos);
    CHECK(bytes.find("\"model\"") < bytes.find("\"output_config\""));
    CHECK(bytes.find("\"output_config\"") < bytes.find("\"stream\""));
    CHECK(bytes.find("\"system\"") < bytes.find("\"thinking\""));
    std::set<std::string> keys;
    collectKeys(parsed(bytes), keys);
    CHECK(keys.count("budget_tokens") == 0);
    CHECK(keys.count("display") == 0);

    // Default for both: the bytes of a request that never had the settings (the goldens).
    ChatRequest plain = basicRequest();
    plain.effort = Effort::Default;
    plain.thinking = Thinking::Default;
    CHECK((buildAnthropicRequestJson(plain) == buildAnthropicRequestJson(basicRequest())));
    const json body = parsed(buildAnthropicRequestJson(plain));
    CHECK_FALSE(body.contains("output_config"));
    CHECK_FALSE(body.contains("thinking"));
}

TEST_CASE("request: the system array does not depend on the conversation") {
    // The cache prefix is tools, then system, then messages: the system bytes must stay put while
    // the conversation grows, or every follow-up question would miss the cache.
    ChatRequest shortChat = basicRequest();
    shortChat.conversation = {{Role::User, "Test question one?"}};
    ChatRequest longChat = basicRequest();
    longChat.conversation.clear();
    for (int i = 0; i < 12; ++i) {
        longChat.conversation.push_back({Role::User, "Test question " + std::to_string(i) + "?"});
        longChat.conversation.push_back(
            {Role::Assistant, "Test answer " + std::to_string(i) + "."});
    }
    longChat.conversation.push_back({Role::User, "Test final question?"});

    const json shortBody = parsed(buildAnthropicRequestJson(shortChat));
    const json longBody = parsed(buildAnthropicRequestJson(longChat));
    CHECK((shortBody["system"].dump() == longBody["system"].dump()));
    CHECK(shortBody["messages"].size() != longBody["messages"].size());

    // The same bytes in the raw body too: the system array text is a substring of both.
    const std::string systemBytes = shortBody["system"].dump();
    CHECK(buildAnthropicRequestJson(shortChat).find(systemBytes) != std::string::npos);
    CHECK(buildAnthropicRequestJson(longChat).find(systemBytes) != std::string::npos);

    // A new snapshot changes block 2 only, never block 1.
    ChatRequest nextSnapshot = shortChat;
    nextSnapshot.snapshotText = "Test snapshot text, newer.";
    const json nextBody = parsed(buildAnthropicRequestJson(nextSnapshot));
    CHECK((nextBody["system"][0].dump() == shortBody["system"][0].dump()));
    CHECK((nextBody["system"][1].dump() != shortBody["system"][1].dump()));
}
