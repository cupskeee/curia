// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "probe_options.h"

using namespace curia::probe;

namespace {
ParseResult parse(std::vector<std::string> args) {
    return parseArgs(args);
}
}  // namespace

TEST_CASE("a stage is required") {
    const auto result = parse({});
    CHECK_FALSE(result.ok);
    CHECK(result.error.find("--stage") != std::string::npos);
}

TEST_CASE("defaults") {
    const auto result = parse({"--stage", "e3"});
    REQUIRE(result.ok);
    CHECK(result.options.stage == Stage::E3);
    CHECK(result.options.combo == 0);
    CHECK(result.options.stepSeconds == doctest::Approx(5.0));
    CHECK(result.options.maxSeconds == doctest::Approx(900.0));
    CHECK(result.options.startDelaySeconds == doctest::Approx(15.0));
    CHECK(result.options.displayIndex == -1);
    CHECK(result.options.sound);
    CHECK_FALSE(result.options.echo);
    CHECK(result.options.resultsDir.empty());
}

TEST_CASE("every stage name parses and round-trips") {
    for (const char* name : {"e1", "e2", "e3", "e4", "interactive", "detect", "tray"}) {
        const auto result = parse({"--stage", name});
        REQUIRE(result.ok);
        CHECK(std::string(stageName(result.options.stage)) == name);
    }
    CHECK_FALSE(parse({"--stage", "e5"}).ok);
}

TEST_CASE("both --key value and --key=value work") {
    const auto result = parse({"--stage=e4", "--combo", "7", "--step-seconds=2.5", "--no-sound",
                               "--echo", "--results-dir", "/tmp/out"});
    REQUIRE(result.ok);
    CHECK(result.options.stage == Stage::E4);
    CHECK(result.options.combo == 7);
    CHECK(result.options.stepSeconds == doctest::Approx(2.5));
    CHECK_FALSE(result.options.sound);
    CHECK(result.options.echo);
    CHECK(result.options.resultsDir == "/tmp/out");
}

TEST_CASE("bad values are rejected with a message") {
    CHECK_FALSE(parse({"--stage", "e3", "--combo", "0"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--combo", "21"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--combo", "2.5"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--combo", "x"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--step-seconds", "0"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--seconds", "-1"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--max-seconds", "0"}).ok);
    CHECK_FALSE(parse({"--stage", "tray", "--tray-delay", "-1"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--start-delay", "-1"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--display", "-1"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--display", "1.5"}).ok);
    CHECK_FALSE(parse({"--stage"}).ok);
    CHECK_FALSE(parse({"--stage", "e3", "--bogus"}).ok);
    CHECK(parse({"--stage", "e3", "--bogus"}).error.find("--bogus") != std::string::npos);
}

TEST_CASE("start delay and display") {
    const auto result = parse({"--stage", "e3", "--start-delay", "0", "--display", "1"});
    REQUIRE(result.ok);
    CHECK(result.options.startDelaySeconds == doctest::Approx(0.0));
    CHECK(result.options.displayIndex == 1);
}

TEST_CASE("macOS process serial number arguments are ignored") {
    CHECK(parse({"-psn_0_12345", "--stage", "e1"}).ok);
}

TEST_CASE("runSeconds: stage defaults and overrides") {
    ProbeOptions options;
    options.stage = Stage::E1;
    CHECK(runSeconds(options) == doctest::Approx(20.0));
    options.stage = Stage::E3;
    options.combo = 0;
    CHECK(runSeconds(options) == doctest::Approx(105.0));  // 20 combinations plus a replay of 1
    options.stepSeconds = 2.0;
    CHECK(runSeconds(options) == doctest::Approx(42.0));
    options.combo = 3;  // hold one combination
    CHECK(runSeconds(options) == doctest::Approx(60.0));
    options.stage = Stage::Interactive;
    CHECK(runSeconds(options) == doctest::Approx(0.0));
    options.seconds = 30.0;
    CHECK(runSeconds(options) == doctest::Approx(30.0));
    options.stage = Stage::Detect;
    options.seconds = 0.0;
    CHECK(runSeconds(options) == doctest::Approx(60.0));
}
