// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <chrono>
#include <string>

#include "probe_log.h"

using namespace curia::probe;

TEST_CASE("kv quotes values only when needed") {
    CHECK(kv("a", std::string_view("b")) == "a=b");
    CHECK(kv("a", std::string_view("")) == "a=\"\"");
    CHECK(kv("name", std::string_view("two words")) == "name=\"two words\"");
    CHECK(kv("q", std::string_view("say \"hi\"")) == "q=\"say \\\"hi\\\"\"");
    CHECK(kv("eq", std::string_view("a=b")) == "eq=\"a=b\"");
    CHECK(kv("nl", std::string_view("a\nb")) == "nl=\"a\\nb\"");
    CHECK(kv("n", 42LL) == "n=42");
    CHECK(kv("x", 1.5, 2) == "x=1.50");
}

TEST_CASE("fields joins with single spaces") {
    CHECK(fields({}).empty());
    CHECK(fields({"a=1"}) == "a=1");
    CHECK(fields({"a=1", "b=2", "c=3"}) == "a=1 b=2 c=3");
}

TEST_CASE("timestamps and lines have a fixed shape") {
    using namespace std::chrono;
    const auto time = system_clock::time_point(milliseconds(1'759'672'989'123LL));
    CHECK(ProbeLog::formatTimestamp(time, true) == "2025-10-05 14:03:09.123");
    CHECK(ProbeLog::formatLine("T", "event", "a=1") == "T event a=1\n");
    CHECK(ProbeLog::formatLine("T", "event", "") == "T event\n");
}
