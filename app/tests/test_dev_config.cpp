// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <map>
#include <ostream>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "core/dev_config.h"

using curia::DevConfig;
using curia::SecretRegistry;

namespace {

// Variable names are built from parts here only to keep this file independent of the production
// spelling rules; tests may spell them out (the source rule allows app/tests/).
const char* const kKeyName = "CURIA_ANTHROPIC_API_KEY";
const char* const kModelName = "CURIA_MODEL";

std::string fakeKey(const std::string& tail = "0123456789abcdefghij") {
    return std::string("sk-ant-") + "FAKE" + tail;
}

// A fake environment that remembers which names were asked for.
struct FakeEnv {
    std::map<std::string, std::string> values;
    mutable std::set<std::string> asked;

    curia::platform::GetEnv getter() const {
        return [this](const char* name) {
            asked.insert(name);
            const auto found = values.find(name);
            return found != values.end() ? found->second : std::string();
        };
    }
};

// Removes a key from the process-wide registry whichever way a test case ends.
struct ProcessKeyGuard {
    explicit ProcessKeyGuard(std::string text) : key(std::move(text)) {}
    ~ProcessKeyGuard() { SecretRegistry::instance().remove(key); }
    ProcessKeyGuard(const ProcessKeyGuard&) = delete;
    ProcessKeyGuard& operator=(const ProcessKeyGuard&) = delete;
    std::string key;
};

}  // namespace

#if defined(CURIA_ENABLE_DEV_ENV)

TEST_CASE("dev config: content logging and the transcript folder") {
    FakeEnv env;
    env.values = {{"CURIA_LOG_CONTENT", "1"}, {"CURIA_TRANSCRIPT_DIR", "/some/folder"}};
    SecretRegistry registry;
    DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK(config.logContent);
    CHECK(config.transcriptDir == "/some/folder");
    CHECK(config.warnings.empty());

    env.values = {};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.logContent);
    CHECK_FALSE(config.transcriptDir.has_value());

    // Only exactly 1 turns it on; a wrong value is noted, 0 is a quiet no.
    for (const char* value : {"true", "yes", "2", "01"}) {
        env.values = {{"CURIA_LOG_CONTENT", value}};
        config = curia::loadDevConfig(env.getter(), registry);
        CHECK_FALSE(config.logContent);
        REQUIRE(config.warnings.size() == 1);
        CHECK(config.warnings[0].find("CURIA_LOG_CONTENT") == 0);
        CHECK(config.warnings[0].find(value) == std::string::npos);
    }
    env.values = {{"CURIA_LOG_CONTENT", "0"}};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.logContent);
    CHECK(config.warnings.empty());

    env.values = {{"CURIA_TRANSCRIPT_DIR", std::string("bad\x01path")}};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.transcriptDir.has_value());
    REQUIRE(config.warnings.size() == 1);
    CHECK(config.warnings[0].find("bad") == std::string::npos);
    // A warning is not a reason to call the setup incomplete.
    env.values = {
        {kKeyName, fakeKey()}, {kModelName, "mock-model-1"}, {"CURIA_LOG_CONTENT", "yes"}};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK(curia::describeNotConfigured(config).empty());
    CHECK(curia::missingVariables(config).empty());
}

TEST_CASE("dev config: missingVariables names what is missing, never a value") {
    FakeEnv env;
    SecretRegistry registry;
    DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK(curia::missingVariables(config) == std::vector<std::string>{kKeyName, kModelName});
    env.values = {{kKeyName, fakeKey()}};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK(curia::missingVariables(config) == std::vector<std::string>{kModelName});
    env.values = {{kKeyName, "short"}, {kModelName, "mock-model-1"}};
    config = curia::loadDevConfig(env.getter(), registry);
    CHECK(curia::missingVariables(config) == std::vector<std::string>{kKeyName});
    CHECK(std::string(curia::keyVariableName()) == kKeyName);
    CHECK(std::string(curia::modelVariableName()) == kModelName);
}

namespace {

bool anyContains(const DevConfig& config, const std::string& needle) {
    for (const std::string& problem : config.problems) {
        if (problem.find(needle) != std::string::npos) {
            return true;
        }
    }
    return curia::describeNotConfigured(config).find(needle) != std::string::npos;
}

}  // namespace

TEST_CASE("dev config: a valid key and model") {
    FakeEnv env;
    env.values = {{kKeyName, fakeKey()}, {kModelName, "mock-model-1"}};
    SecretRegistry registry;
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    REQUIRE(config.apiKey.has_value());
    if (config.apiKey.has_value()) {  // (the static analyser does not know REQUIRE)
        CHECK(config.apiKey->reveal() == fakeKey());
    }
    REQUIRE(config.model.has_value());
    if (config.model.has_value()) {
        CHECK(*config.model == "mock-model-1");
    }
    CHECK(config.problems.empty());
    CHECK(curia::describeNotConfigured(config).empty());
    // Only the four development variables were asked for, and the key is now scrubbed from any
    // text.
    CHECK(env.asked ==
          std::set<std::string>{kKeyName, kModelName, "CURIA_LOG_CONTENT", "CURIA_TRANSCRIPT_DIR"});
    CHECK(registry.size() == 1);
    CHECK(registry.redact("x " + fakeKey() + " y") == "x [redacted] y");
}

TEST_CASE("dev config: the process registry is used when none is given") {
    FakeEnv env;
    const std::string key = fakeKey("process-registry-key-0001");
    const ProcessKeyGuard guard(key);
    env.values = {{kKeyName, key}, {kModelName, "mock-model-1"}};
    const DevConfig config = curia::loadDevConfig(env.getter());
    REQUIRE(config.apiKey.has_value());
    CHECK(curia::redact("k " + key) == "k [redacted]");
}

TEST_CASE("dev config: a key with a quote or backslash is scrubbed from JSON text too") {
    FakeEnv env;
    const std::string key = "abcd\"efgh\\ijkl";
    env.values = {{kKeyName, key}};
    SecretRegistry registry;
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    REQUIRE(config.apiKey.has_value());
    CHECK(registry.redact("raw " + key + " end") == "raw [redacted] end");
    CHECK(registry.redact("json abcd\\\"efgh\\\\ijkl end") == "json [redacted] end");
    CHECK(registry.size() == 2);
}

TEST_CASE("dev config: nothing set is not a problem, but a message names what is missing") {
    FakeEnv env;
    SecretRegistry registry;
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.apiKey.has_value());
    CHECK_FALSE(config.model.has_value());
    CHECK(config.problems.empty());
    const std::string message = curia::describeNotConfigured(config);
    CHECK(message.find(kKeyName) != std::string::npos);
    CHECK(message.find(kModelName) != std::string::npos);
    CHECK(message.find("not set") != std::string::npos);

    // Empty values count as unset.
    env.values = {{kKeyName, ""}, {kModelName, ""}};
    const DevConfig emptyValues = curia::loadDevConfig(env.getter(), registry);
    CHECK(emptyValues.problems.empty());
    CHECK_FALSE(emptyValues.apiKey.has_value());
}

TEST_CASE("dev config: only one of the two") {
    SecretRegistry registry;
    FakeEnv onlyKey;
    onlyKey.values = {{kKeyName, fakeKey()}};
    const DevConfig a = curia::loadDevConfig(onlyKey.getter(), registry);
    CHECK(a.apiKey.has_value());
    const std::string messageA = curia::describeNotConfigured(a);
    CHECK(messageA.find(kModelName) != std::string::npos);
    CHECK(messageA.find(kKeyName) == std::string::npos);

    FakeEnv onlyModel;
    onlyModel.values = {{kModelName, "mock-model-1"}};
    const DevConfig b = curia::loadDevConfig(onlyModel.getter(), registry);
    CHECK(b.model.has_value());
    const std::string messageB = curia::describeNotConfigured(b);
    CHECK(messageB.find(kKeyName) != std::string::npos);
    CHECK(messageB.find(kModelName) == std::string::npos);

    // One variable named: "Set it"; both: "Set them".
    for (const std::string& message : {messageA, messageB}) {
        CHECK(message.find(" Set it in the environment") != std::string::npos);
        CHECK(message.find("Set them") == std::string::npos);
    }
    const FakeEnv nothing;
    const std::string both =
        curia::describeNotConfigured(curia::loadDevConfig(nothing.getter(), registry));
    CHECK(both.find(" Set them in the environment") != std::string::npos);
    // An unusable key and a missing model are two variables too.
    FakeEnv badKey;
    badKey.values = {{kKeyName, "short"}};
    const std::string badKeyMessage =
        curia::describeNotConfigured(curia::loadDevConfig(badKey.getter(), registry));
    CHECK(badKeyMessage.find(" Set them in the environment") != std::string::npos);
    // An unusable model with a usable key: one.
    FakeEnv badModel;
    badModel.values = {{kKeyName, fakeKey()}, {kModelName, "has space"}};
    const std::string badModelMessage =
        curia::describeNotConfigured(curia::loadDevConfig(badModel.getter(), registry));
    CHECK(badModelMessage.find(kModelName) != std::string::npos);
    CHECK(badModelMessage.find(" Set it in the environment") != std::string::npos);
}

TEST_CASE("dev config: model rules") {
    SecretRegistry registry;
    const std::vector<std::string> good = {"a", "mock-model-1", "vendor:model.v2_x", "A-Z.0_9:-",
                                           std::string(128, 'm')};
    for (const std::string& model : good) {
        FakeEnv env;
        env.values = {{kModelName, model}};
        const DevConfig config = curia::loadDevConfig(env.getter(), registry);
        REQUIRE(config.model.has_value());
        if (config.model.has_value()) {
            CHECK(*config.model == model);
        }
        CHECK(config.problems.empty());
    }
    const std::vector<std::string> bad = {
        std::string(129, 'm'),
        "has space",
        " leading",
        "trailing ",
        "line\nbreak",
        "carriage\rreturn",
        "tab\there",
        std::string("nul\0byte", 8),
        "caf\xc3\xa9",
        "slash/model",
        "quote\"model",
        "semi;colon",
        "emoji-\xf0\x9f\x98\x80",
        "fullwidth\xef\xbc\xa1",
    };
    for (const std::string& model : bad) {
        FakeEnv env;
        env.values = {{kModelName, model}};
        const DevConfig config = curia::loadDevConfig(env.getter(), registry);
        CHECK_FALSE(config.model.has_value());
        REQUIRE(config.problems.size() == 1);
        CHECK(config.problems[0].find(kModelName) != std::string::npos);
        CHECK_FALSE(anyContains(config, model));  // the value is never echoed
    }
}

TEST_CASE("dev config: key rules, and a bad key is never repeated or registered") {
    const std::vector<std::string> bad = {
        fakeKey() + " extra",
        " " + fakeKey(),
        fakeKey() + "\n",
        fakeKey() + "\r\n",
        fakeKey().substr(0, 12) + "\tTAB" + fakeKey().substr(12),
        fakeKey() + "\xc3\xa9",  // non-ASCII
        fakeKey() + "\xc2\xa0",  // no-break space
        std::string("sk-ant-") + "FAKE" + std::string("\0nul-tail-key", 13),
        std::string("\x01") + fakeKey(),     // control character
        fakeKey() + std::string("\x7f"),     // DEL
        fakeKey() + std::string(4096, 'k'),  // over 4096 bytes
        "short7!",                           // too short to be scrubbed from logs
    };
    for (const std::string& key : bad) {
        FakeEnv env;
        env.values = {{kKeyName, key}, {kModelName, "mock-model-1"}};
        SecretRegistry registry;
        const DevConfig config = curia::loadDevConfig(env.getter(), registry);
        CHECK_FALSE(config.apiKey.has_value());
        REQUIRE(config.problems.size() == 1);
        CHECK(config.problems[0].find(kKeyName) != std::string::npos);
        CHECK(registry.size() == 0);
        // Search every string the module can hand out for the value, and for pieces of it.
        CHECK_FALSE(anyContains(config, key));
        CHECK_FALSE(anyContains(config, key.substr(0, 8)));
        CHECK(config.model.has_value());  // the model is judged on its own
    }
}

TEST_CASE("dev config: key length limits") {
    SecretRegistry registry;
    FakeEnv env;
    env.values = {{kKeyName, std::string(8, 'k')}};
    CHECK(curia::loadDevConfig(env.getter(), registry).apiKey.has_value());
    env.values = {{kKeyName, std::string(4096, 'k')}};
    CHECK(curia::loadDevConfig(env.getter(), registry).apiKey.has_value());
    env.values = {{kKeyName, std::string(4097, 'k')}};
    CHECK_FALSE(curia::loadDevConfig(env.getter(), registry).apiKey.has_value());
    env.values = {{kKeyName, std::string(7, 'k')}};
    CHECK_FALSE(curia::loadDevConfig(env.getter(), registry).apiKey.has_value());
    // A key that is a part of the redaction marker cannot be protected.
    env.values = {{kKeyName, "redacted"}};
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.apiKey.has_value());
    CHECK(config.problems.size() == 1);
}

TEST_CASE("dev config: a full registry refuses the key instead of leaving it unscrubbed") {
    SecretRegistry registry;
    for (std::size_t i = 0; i < SecretRegistry::kMaxSecrets; ++i) {
        REQUIRE(registry.add("filler-secret-" + std::to_string(i)));
    }
    FakeEnv env;
    env.values = {{kKeyName, fakeKey()}};
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.apiKey.has_value());
    CHECK(config.problems.size() == 1);
    CHECK_FALSE(anyContains(config, fakeKey()));
}

TEST_CASE("dev config: an empty lookup function reads nothing") {
    SecretRegistry registry;
    const DevConfig config = curia::loadDevConfig(curia::platform::GetEnv(), registry);
    CHECK_FALSE(config.apiKey.has_value());
    CHECK_FALSE(config.model.has_value());
}

#else  // built without CURIA_ENABLE_DEV_ENV

TEST_CASE("dev config: disabled in this build, the environment is never read") {
    FakeEnv env;
    env.values = {{kKeyName, fakeKey()}, {kModelName, "mock-model-1"}};
    SecretRegistry registry;
    const DevConfig config = curia::loadDevConfig(env.getter(), registry);
    CHECK_FALSE(config.apiKey.has_value());
    CHECK_FALSE(config.model.has_value());
    REQUIRE(config.problems.size() == 1);
    CHECK(config.problems[0] == curia::kDevConfigDisabledProblem);
    CHECK(env.asked.empty());
    CHECK(registry.size() == 0);
    const std::string message = curia::describeNotConfigured(config);
    CHECK(message.find("disabled in this build") != std::string::npos);
    CHECK(message.find(kKeyName) == std::string::npos);
    CHECK(message.find(fakeKey()) == std::string::npos);
    CHECK(curia::missingVariables(config) == std::vector<std::string>{kKeyName, kModelName});
    CHECK_FALSE(config.logContent);
    CHECK_FALSE(config.transcriptDir.has_value());

    // The no-argument overload does not touch the real environment either.
    const DevConfig real = curia::loadDevConfig();
    CHECK_FALSE(real.apiKey.has_value());
    CHECK(real.problems.size() == 1);
}

#endif
