// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <sstream>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge_test_support.h"
#include "core/watch_log.h"

using namespace curia::bridge::test;

namespace {

// Runs the headless mode with its output going to strings.
struct Run {
    int exitCode = -1;
    std::vector<nlohmann::json> events;
    std::string err;
};

Run runMode(const curia::CliOptions& options) {
    std::ostringstream out;
    std::ostringstream err;
    Run run;
    run.exitCode = curia::runWatchLog(options, out, err);
    std::istringstream lines(out.str());
    std::string line;
    while (std::getline(lines, line)) {
        run.events.push_back(nlohmann::json::parse(line));
    }
    run.err = err.str();
    return run;
}

curia::CliOptions watchOptions(const std::string& path) {
    curia::CliOptions options;
    options.watchLogPath = path;
    options.fromStart = true;
    options.timeoutSeconds = 20;
    return options;
}

}  // namespace

TEST_CASE("watch mode: prints one JSON line per event and exits 0 after N complete frames") {
    const TempDir dir;
    const std::string log = dir.path("debug.log");
    // A damaged frame, then two good ones, then a third that must not be printed.
    std::vector<std::string> damaged = exampleFrame(1);
    damaged.back() = "CURIA1|1|END|99";
    REQUIRE(writeFile(log, stream(damaged) + stream(exampleFrame(2)) + stream(exampleFrame(3)) +
                               stream(exampleFrame(4))));
    curia::CliOptions options = watchOptions(log);
    options.maxFrames = 2;
    const Run run = runMode(options);
    CHECK(run.exitCode == 0);
    REQUIRE(run.events.size() == 3);
    CHECK(run.events[0]["event"] == "frame_damaged");
    CHECK(run.events[0]["reason"] == "count_mismatch");
    CHECK(run.events[1]["event"] == "frame_complete");
    CHECK(run.events[1]["frame"]["snap"] == 2);
    CHECK(run.events[2]["frame"]["snap"] == 3);
    CHECK(run.err.find("curia: log attached") != std::string::npos);
    CHECK(run.err.find("curia: offset ") != std::string::npos);
}

TEST_CASE("watch mode: exit 3 when the timeout comes before the asked-for frames") {
    const TempDir dir;
    const std::string log = dir.path("debug.log");
    REQUIRE(writeFile(log, stream(exampleFrame(1))));
    curia::CliOptions options = watchOptions(log);
    options.maxFrames = 2;
    options.timeoutSeconds = 1;
    const Run run = runMode(options);
    CHECK(run.exitCode == 3);
    CHECK(run.events.size() == 1);
    CHECK(run.err.find("timeout") != std::string::npos);
}

TEST_CASE("watch mode: without a frame count it runs to the timeout and exits 0") {
    const TempDir dir;
    curia::CliOptions options = watchOptions(dir.path("never_created.log"));
    options.timeoutSeconds = 1;
    const Run run = runMode(options);
    CHECK(run.exitCode == 0);
    CHECK(run.events.empty());
    CHECK(run.err.find("curia: log missing") != std::string::npos);
}

TEST_CASE("watch mode: a log that already exists is not replayed unless --from-start") {
    const TempDir dir;
    const std::string log = dir.path("debug.log");
    REQUIRE(writeFile(log, stream(exampleFrame(1))));
    curia::CliOptions options = watchOptions(log);
    options.fromStart = false;
    options.timeoutSeconds = 1;
    const Run run = runMode(options);
    CHECK(run.exitCode == 0);
    CHECK(run.events.empty());
}
