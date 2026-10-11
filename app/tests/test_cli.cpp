// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <vector>

#include "core/cli.h"

namespace {

bool parse(std::vector<const char*> args, curia::CliOptions& out, std::string& error) {
    args.insert(args.begin(), "curia");
    return curia::parseCli(static_cast<int>(args.size()), args.data(), out, error);
}

}  // namespace

TEST_CASE("no arguments runs the normal app") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({}, options, error));
    CHECK_FALSE(options.smoke);
    CHECK(options.idleSeconds == 0);
}

TEST_CASE("flags are recognised") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--smoke", "--version"}, options, error));
    CHECK(options.smoke);
    CHECK(options.showVersion);
}

TEST_CASE("--idle-test needs a sensible number") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--idle-test", "10"}, options, error));
    CHECK(options.idleSeconds == 10);
    CHECK_FALSE(parse({"--idle-test"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "0"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "abc"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "5x"}, options, error));
}

TEST_CASE("unknown arguments and conflicting modes are rejected") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--nope"}, options, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parse({"--smoke", "--idle-test", "3"}, options, error));
}

TEST_CASE("--watch-log and its companions") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--watch-log", "some.log"}, options, error));
    CHECK(options.watchLogPath == "some.log");
    CHECK_FALSE(options.fromStart);
    CHECK(options.maxFrames == 0);
    CHECK(options.timeoutSeconds == 0);

    CHECK(parse(
        {"--watch-log", "a.log", "--from-start", "--max-frames", "3", "--timeout-seconds", "20"},
        options, error));
    CHECK(options.fromStart);
    CHECK(options.maxFrames == 3);
    CHECK(options.timeoutSeconds == 20);
    // Order does not matter.
    CHECK(parse({"--max-frames", "1", "--watch-log", "a.log"}, options, error));
    CHECK(options.maxFrames == 1);
}

TEST_CASE("--watch-log needs a path and sensible numbers") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--watch-log"}, options, error));
    CHECK_FALSE(parse({"--watch-log", ""}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames", "0"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames", "x"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "0"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "5s"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "86401"}, options, error));
}

TEST_CASE("--watch-log conflicts with the window modes, and its options need it") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--watch-log", "a.log", "--smoke"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "3", "--watch-log", "a.log"}, options, error));
    CHECK_FALSE(parse({"--from-start"}, options, error));
    CHECK_FALSE(parse({"--max-frames", "2"}, options, error));
    CHECK_FALSE(parse({"--timeout-seconds", "2"}, options, error));
}
