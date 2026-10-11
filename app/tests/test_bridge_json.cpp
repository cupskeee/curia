// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include <nlohmann/json.hpp>

#include "bridge/bridge_pipeline.h"
#include "bridge/frame_json.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using namespace curia::bridge::test;

TEST_CASE("json: a frame keeps its documented keys; absent sections have no key") {
    BridgePipeline pipeline;
    const auto events = pipeline.feed(stream({
        "CURIA1|3|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One",
        "CURIA1|3|HEIR|none",
        "CURIA1|3|END|3",
    }));
    REQUIRE(events.size() == 1);
    const nlohmann::json json = toJson(events);
    REQUIRE(json.is_array());
    REQUIRE(json.size() == 1);
    const nlohmann::json& event = json.at(0);
    CHECK(event.at("event") == "frame_complete");
    const nlohmann::json& frame = event.at("frame");
    CHECK(frame.at("snap") == 3);
    CHECK(frame.at("mod_version") == "0.1.0");
    CHECK(frame.at("date").at("year") == 1066);
    CHECK(frame.at("ruler").at("name") == "Test Ruler One");
    CHECK(frame.at("heir") == nlohmann::json{{"none", true}});
    CHECK(frame.contains("text_shortened"));
    CHECK(frame.contains("record_count"));
    CHECK_FALSE(frame.contains("fin"));
    CHECK_FALSE(frame.contains("vassals"));
    CHECK_FALSE(frame.contains("wars"));
    CHECK_FALSE(frame.contains("claims"));
    CHECK_FALSE(frame.contains("factions"));
}

TEST_CASE("json: the output is deterministic and ends in a newline") {
    const auto events = runBytes(stream(exampleFrame(7)));
    const std::string a = eventsText(events);
    const std::string b = eventsText(runBytes(stream(exampleFrame(7))));
    CHECK(a == b);
    CHECK(a.back() == '\n');
    CHECK(a.find("\n\n") == std::string::npos);
}

TEST_CASE("json: damage, unsupported-format and diagnostic events") {
    const auto events = runBytes(stream({
        "CURIA1|3|BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One",
        "CURIA1|3|END|9",
        "CURIA1|4|BEGIN|1066.9.15|Test Ruler One",
        "CURIA1|0|FIN|1|2",
        "CURIA1|abc|FIN|1|2",
    }));
    const nlohmann::json json = toJson(events);
    REQUIRE(json.size() == 4);
    CHECK(json.at(0).at("event") == "frame_damaged");
    CHECK(json.at(0).at("reason") == "count_mismatch");
    CHECK(json.at(0).at("snap") == 3);
    CHECK(json.at(1).at("event") == "unsupported_format");
    CHECK(json.at(1).at("leading_fields") == "1066.9.15|Test Ruler One");
    CHECK(json.at(2).at("event") == "diagnostic");
    CHECK(json.at(2).at("reason") == "snap_rule");
    CHECK(json.at(2).at("snap") == 0);
    CHECK(json.at(3).at("snap").is_null());
}

TEST_CASE("json: bytes that are not UTF-8 never make the output throw") {
    // The leading fields of an unsupported BEGIN are raw bytes.
    const auto events = runBytes(prefixedLine("CURIA1|4|BEGIN|\xFF\xFE|x"));
    REQUIRE(events.size() == 1);
    const std::string text = eventsText(events);
    CHECK_FALSE(text.empty());
    const nlohmann::json parsed = nlohmann::json::parse(text, nullptr, false);
    CHECK_FALSE(parsed.is_discarded());
}

TEST_CASE("json: reason names") {
    CHECK(std::string(toString(DamageReason::Incomplete)) == "incomplete");
    CHECK(std::string(toString(DamageReason::InvalidRecord)) == "invalid_record");
    CHECK(std::string(toString(DamageReason::SnapMismatch)) == "snap_mismatch");
    CHECK(std::string(toString(DamageReason::CountMismatch)) == "count_mismatch");
    CHECK(std::string(toString(DamageReason::Order)) == "order");
    CHECK(std::string(toString(DamageReason::TooLarge)) == "too_large");
    CHECK(std::string(toString(DamageReason::TruncatedLog)) == "truncated_log");
    CHECK(std::string(toString(InvalidReason::Header)) == "header");
    CHECK(std::string(toString(InvalidReason::Oversize)) == "oversize");
}
