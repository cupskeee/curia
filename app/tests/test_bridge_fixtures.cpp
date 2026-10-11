// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge/frame_json.h"
#include "bridge_test_support.h"
#include "platform/env.h"

using namespace curia::bridge;
using namespace curia::bridge::test;

namespace {

// A short tag for each event, so that the expectation table below can be read at a glance:
//   C<snap>            a Complete frame
//   D<snap>:<reason>   a Damaged frame
//   U<snap>            an unsupported-format report
//   G                  a diagnostic (an invalid record that is only reported to the log)
std::vector<std::string> tags(const std::vector<Event>& events) {
    std::vector<std::string> out;
    for (const Event& event : events) {
        if (const auto* c = std::get_if<FrameComplete>(&event)) {
            out.push_back("C" + std::to_string(c->frame.snap));
        } else if (const auto* d = std::get_if<FrameDamaged>(&event)) {
            out.push_back("D" + std::to_string(d->snap) + ":" + toString(d->reason));
        } else if (const auto* u = std::get_if<UnsupportedFormat>(&event)) {
            out.push_back("U" + std::to_string(u->snap));
        } else {
            out.emplace_back("G");
        }
    }
    return out;
}

// What each fixture must produce, written from the spec (not from the program's output).
const std::map<std::string, std::vector<std::string>>& expectations() {
    static const std::map<std::string, std::vector<std::string>> table = {
        {"example_frame", {"C7"}},
        {"minimal_frame", {"C1"}},
        {"sections_full", {"C12"}},
        {"sections_empty", {"C2"}},
        {"negative_zero", {"C3"}},
        {"none_forms", {"C4"}},
        {"factions_all_types", {"C5"}},
        {"war_roles", {"C6"}},
        {"claims_kinds", {"C8"}},
        {"damaged_invalid_record", {"D20:invalid_record", "C21"}},
        {"damaged_snap_mismatch", {"D30:snap_mismatch", "C32", "D33:snap_mismatch", "C35"}},
        {"damaged_count_mismatch", {"D40:count_mismatch", "D41:count_mismatch", "C42"}},
        {"damaged_group_count",
         {"D50:count_mismatch", "D51:count_mismatch", "D52:count_mismatch", "D53:count_mismatch",
          "D54:count_mismatch", "C55"}},
        {"damaged_order", {"D60:order", "D61:order", "D62:order", "D63:order", "D64:order", "C65"}},
        {"damaged_incomplete", {"D70:incomplete", "C71", "D72:incomplete", "U73", "C74"}},
        {"damaged_truncated", {"D80:truncated_log", "C1", "C1"}},
        {"frame_512_records", {"C90"}},
        {"noise_interleaved", {"C100"}},
        {"name_with_pipe", {"C110"}},
        {"markup_sample", {"C120"}},
        {"unknown_types", {"C130"}},
        {"probe_in_frame", {"C140"}},
        {"load_records", {"C150"}},
        {"snap_falling_repeated", {"C7", "C3", "C3", "C1"}},
        {"old_layout_begin", {"G", "U1", "C2"}},
        {"oversize_name", {"C160"}},
        {"snap0_records", {"G", "G", "G", "G", "G", "C170"}},
        {"double_end", {"C180"}},
        {"marker_offset0", {"D191:count_mismatch"}},
        {"invalid_records_idle", {"G", "G",  "G", "G", "G", "G", "G", "G", "G", "G",
                                  "G", "U7", "G", "G", "G", "G", "G", "G", "G", "G"}},
        {"valid_notable", {"C8"}},
    };
    return table;
}

std::string goldenPath(const std::string& name) {
    return fixtureDir() + "/" + name + ".events.json";
}

}  // namespace

TEST_CASE("fixtures: every fixture has an expectation, a golden file and loads cleanly") {
    const auto names = fixtureNames();
    CHECK(names.size() == expectations().size());
    for (const std::string& name : names) {
        CAPTURE(name);
        CHECK(expectations().count(name) == 1);
        const Fixture fixture = loadFixture(name);
        CHECK(fixture.errors.empty());
        CHECK_FALSE(fixture.actions.empty());
    }
}

TEST_CASE("fixtures: event sequences match the spec") {
    for (const auto& [name, expected] : expectations()) {
        CAPTURE(name);
        const Fixture fixture = loadFixture(name);
        REQUIRE(fixture.errors.empty());
        CHECK(tags(runActions(fixture.actions, 0)) == expected);
    }
}

TEST_CASE("fixtures: golden outputs") {
    // CURIA_UPDATE_GOLDEN=1 rewrites the golden files; review the diff against the spec by hand.
    const bool update = !curia::platform::getEnv("CURIA_UPDATE_GOLDEN").empty();
    for (const std::string& name : fixtureNames()) {
        CAPTURE(name);
        const Fixture fixture = loadFixture(name);
        REQUIRE(fixture.errors.empty());
        const std::string actual = eventsText(runActions(fixture.actions, 0));
        if (update) {
            REQUIRE(writeFile(goldenPath(name), actual));
            continue;
        }
        bool ok = false;
        const std::string golden = readFile(goldenPath(name), ok);
        REQUIRE_MESSAGE(ok, "missing golden file");
        const nlohmann::json expectedJson = nlohmann::json::parse(golden, nullptr, false);
        const nlohmann::json actualJson = nlohmann::json::parse(actual, nullptr, false);
        REQUIRE_FALSE(expectedJson.is_discarded());
        if (expectedJson != actualJson) {
            INFO("expected:\n" << golden << "\nactual:\n" << actual);
            FAIL("golden mismatch");
        }
    }
}

TEST_CASE("fixtures: chunk boundaries do not change the events") {
    for (const std::string& name : fixtureNames()) {
        CAPTURE(name);
        const Fixture fixture = loadFixture(name);
        REQUIRE(fixture.errors.empty());
        const std::string whole = eventsText(runActions(fixture.actions, 0));
        for (const std::size_t chunk : {1U, 2U, 3U, 7U, 64U}) {
            CAPTURE(chunk);
            CHECK(eventsText(runActions(fixture.actions, chunk)) == whole);
        }
        for (const std::uint32_t seed : {1U, 2U, 3U, 12345U, 987654321U}) {
            CAPTURE(seed);
            CHECK(eventsText(runActions(fixture.actions, 0, seed)) == whole);
        }
    }
}

TEST_CASE("fixtures: the markup sample really contains the raw 0x15 byte") {
    bool ok = false;
    const std::string text = readFile(fixtureDir() + "/markup_sample.log", ok);
    REQUIRE(ok);
    CHECK(text.find('\x15') != std::string::npos);
}

TEST_CASE("fixtures: the markup sample is cleaned") {
    const Fixture fixture = loadFixture("markup_sample");
    const auto events = runActions(fixture.actions, 0);
    REQUIRE(events.size() == 1);
    const auto* complete = std::get_if<FrameComplete>(events.data());
    REQUIRE(complete != nullptr);
    const Frame& frame = complete->frame;
    REQUIRE(frame.vassals.has_value());
    CHECK(must(frame.vassals).listed.at(0).name == "Install Test Ruler on the Test Realm Throne");
    REQUIRE(frame.claims.has_value());
    CHECK(must(frame.claims).listed.at(0).title == "Test Realm");
    REQUIRE(frame.wars.has_value());
    CHECK(must(frame.wars).wars.at(0).name == "Test Name");
    CHECK(must(frame.wars).wars.at(0).attacker.name == "ax");
    CHECK(must(frame.wars).wars.at(0).defender.name == "d");
}

TEST_CASE("fixtures: the name containing the delimiter keeps every field in place") {
    const Fixture fixture = loadFixture("name_with_pipe");
    const auto events = runActions(fixture.actions, 0);
    REQUIRE(events.size() == 1);
    const auto* complete = std::get_if<FrameComplete>(events.data());
    REQUIRE(complete != nullptr);
    const Frame& frame = complete->frame;
    CHECK(frame.rulerName == "Test | Ruler | One");
    CHECK(frame.rulerId == 1001);
    REQUIRE(frame.heir.has_value());
    CHECK(must(frame.heir).name == "Test | Heir | Two");
    CHECK(must(frame.heir).opinion == 84);
    REQUIRE(frame.vassals.has_value());
    CHECK(must(frame.vassals).listed.at(0).name == "Test|1|2|3|4|5|Vassal");
    CHECK(must(frame.vassals).listed.at(0).strength == 1523);
    CHECK(must(frame.vassals).listed.at(1).name == "|");
    REQUIRE(frame.claims.has_value());
    CHECK(must(frame.claims).listed.at(0).title == "Test | Realm | CURIA1|110|END|99");
}

TEST_CASE("fixtures: the oversize names are shortened, the frame is kept and flagged") {
    const Fixture fixture = loadFixture("oversize_name");
    const auto events = runActions(fixture.actions, 0);
    REQUIRE(events.size() == 1);
    const auto* complete = std::get_if<FrameComplete>(events.data());
    REQUIRE(complete != nullptr);
    const Frame& frame = complete->frame;
    CHECK(frame.textShortened);
    REQUIRE(frame.vassals.has_value());
    REQUIRE(must(frame.vassals).listed.size() == 2);
    const std::string head1 = "CURIA1|160|VASSAL|1|1003|1|2|3|";
    CHECK(must(frame.vassals).listed[0].name.size() <= 1024 - head1.size());
    CHECK(must(frame.vassals).listed[0].name.size() + 1 >=
          1024 - head1.size());  // trailing space trimmed
    CHECK(must(frame.vassals).listed[0].strength == 3);
    // 3-byte characters: the cut leaves whole characters only.
    const std::string name2 = must(frame.vassals).listed[1].name;
    CHECK(name2.rfind("ab", 0) == 0);
    CHECK((name2.size() - 2) % 3 == 0);
    CHECK(must(frame.vassals).listed[1].strength == 7);
}
