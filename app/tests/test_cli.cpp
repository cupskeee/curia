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
