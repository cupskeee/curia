// SPDX-License-Identifier: MIT
#include "bridge_test_support.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

#include "bridge/frame_json.h"

#ifndef CURIA_FIXTURE_DIR
#error "CURIA_FIXTURE_DIR must point at tests/fixtures"
#endif

namespace curia::bridge::test {

std::string enginePrefix(std::size_t variant) {
    const std::array<std::string, 5> parts = {
        "synthetic-time", "synthetic-level", "synthetic-source:1",
        "synthetic/script/path/file.txt:1", "synthetic_effect_name:"};
    std::string out;
    for (const std::string& part : parts) {
        out += part;
        out += ' ';
    }
    const std::size_t target = 120 + variant % 12;
    while (out.size() + 1 < target) {
        out += '.';
    }
    out += ' ';
    return out;
}

std::string prefixed(std::string_view bareRecord, std::size_t variant) {
    std::string out = enginePrefix(variant);
    out.append(bareRecord);
    return out;
}

std::string prefixedLine(std::string_view bareRecord, std::size_t variant) {
    std::string out = prefixed(bareRecord, variant);
    out += '\n';
    return out;
}

ParseResult parseBare(std::string_view bareRecord, bool overlong) {
    return parseLine(prefixed(bareRecord), overlong);
}

std::string stream(const std::vector<std::string>& bareRecords) {
    std::string out;
    std::size_t i = 0;
    for (const std::string& record : bareRecords) {
        out += prefixedLine(record, i++);
    }
    return out;
}

std::vector<std::string> exampleFrame(std::int32_t snap) {
    const std::string s = std::to_string(snap);
    const std::string p = "CURIA1|" + s + "|";
    return {
        p + "BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One",
        p + "FIN|312|78",
        p + "HEIR|1002|84|1|0|Test Heir Two",
        p + "VASSALS|2|2",
        p + "VASSAL|1|1003|120|55|1523|Test Vassal Three",
        p + "VASSAL|2|1004|40|-12|310|Test Vassal Four",
        p + "WARS|0",
        p + "CLAIMS|1|0|1",
        p + "CLAIM|1|weak|4|Test Realm",
        p + "FACTIONS|1|0|1",
        p + "FACTION|1|INFO|against|3|33|100|2",
        p + "FACTION|1|LEADER|1004|Test Vassal Four",
        p + "FACTION|1|CLAIMANT|1005|Test Claimant Five",
        p + "FACTION|1|TITLE|4|Test Realm",
        p + "FACTION|1|TARGET|1001|Test Ruler One",
        p + "END|16",
    };
}

std::string fixtureDir() {
    return std::string(CURIA_FIXTURE_DIR) + "/curia1";
}

std::vector<std::string> fixtureNames() {
    std::vector<std::string> names;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(fixtureDir(), ec), end; !ec && it != end;
         it.increment(ec)) {
        if (it->path().extension() == ".log") {
            names.push_back(it->path().stem().string());
        }
    }
    std::sort(names.begin(), names.end());
    return names;
}

std::string readFile(const std::string& path, bool& ok) {
    const std::ifstream in(path, std::ios::binary);
    ok = static_cast<bool>(in);
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

bool writeFile(const std::string& path, const std::string& contents) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    return static_cast<bool>(out);
}

Fixture loadFixture(const std::string& name) {
    Fixture fixture;
    fixture.name = name;
    bool ok = false;
    const std::string text = readFile(fixtureDir() + "/" + name + ".log", ok);
    if (!ok) {
        fixture.errors.emplace_back("cannot read the fixture file");
        return fixture;
    }
    std::string pending;
    const auto flush = [&]() {
        if (!pending.empty()) {
            Action action;
            action.bytes = std::move(pending);
            fixture.actions.push_back(std::move(action));
            pending.clear();
        }
    };
    std::size_t lineNumber = 0;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();  // a checkout with CRLF conversion
        }
        if (line.empty() || line[0] == '#') {
            continue;
        }
        if (line == "!truncate") {
            flush();
            Action action;
            action.kind = Action::Kind::Truncate;
            fixture.actions.push_back(std::move(action));
        } else if (line.starts_with("!noise ")) {
            pending += line.substr(7);
            pending += '\n';
        } else if (line.starts_with("!raw ")) {
            pending += line.substr(5);
            pending += '\n';
        } else if (line.starts_with("CURIA1|")) {
            pending += prefixedLine(line, lineNumber);
        } else {
            fixture.errors.push_back("unknown line: " + line);
        }
        ++lineNumber;
    }
    flush();
    return fixture;
}

namespace {

// A tiny deterministic generator (xorshift32): chunk sizes must not depend on the platform's
// <random> implementation.
std::uint32_t nextRandom(std::uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

}  // namespace

std::vector<Event> runActions(const std::vector<Action>& actions, std::size_t chunkSize,
                              std::uint32_t randomSeed) {
    BridgePipeline pipeline;
    std::vector<Event> events;
    std::uint32_t rng = randomSeed == 0 ? 1U : randomSeed;
    const auto append = [&events](std::vector<Event>&& more) {
        events.insert(events.end(), std::make_move_iterator(more.begin()),
                      std::make_move_iterator(more.end()));
    };
    for (const Action& action : actions) {
        if (action.kind == Action::Kind::Truncate) {
            append(pipeline.truncateLog());
            continue;
        }
        const std::string_view bytes = action.bytes;
        if (chunkSize == 0 && randomSeed == 0) {
            append(pipeline.feed(bytes));
            continue;
        }
        std::size_t pos = 0;
        while (pos < bytes.size()) {
            std::size_t take = chunkSize;
            if (randomSeed != 0) {
                take = 1 + nextRandom(rng) % 97;
            }
            take = std::min(take, bytes.size() - pos);
            append(pipeline.feed(bytes.substr(pos, take)));
            pos += take;
        }
    }
    return events;
}

std::vector<Event> runBytes(std::string_view bytes) {
    BridgePipeline pipeline;
    return pipeline.feed(bytes);
}

std::string eventsText(const std::vector<Event>& events) {
    return dumpJson(toJson(events));
}

}  // namespace curia::bridge::test
