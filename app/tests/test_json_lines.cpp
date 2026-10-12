// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/json_lines.h"
#include "core/redact.h"

using curia::JsonLineWriter;
using nlohmann::json;

namespace {

const std::string kKey = std::string("sk-ant-") + "FAKE" + "0123456789abcdef0123";

}  // namespace

TEST_CASE("json lines: one line, compact, ending in a newline") {
    std::ostringstream out;
    JsonLineWriter writer(out);
    writer.write({{"event", "a"}, {"n", 1}});
    writer.write({{"event", "b"}, {"ok", true}});
    const std::string text = out.str();
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    CHECK(text.back() == '\n');
    std::istringstream lines(text);
    std::string line;
    REQUIRE(std::getline(lines, line));
    CHECK(json::parse(line)["event"] == "a");
    REQUIRE(std::getline(lines, line));
    CHECK(json::parse(line)["ok"] == true);
}

TEST_CASE("json lines: control characters are escaped and can not start another line") {
    std::string text = "first\nsecond\r\nthird\tTab\x01\x1f end\x7f";
    text.push_back('\0');
    text += "after nul";
    const std::string line = JsonLineWriter::format({{"event", "token"}, {"text", text}});
    CHECK(line.find('\n') == std::string::npos);
    CHECK(line.find('\r') == std::string::npos);
    for (const char ch : line) {
        CHECK(static_cast<unsigned char>(ch) >= 0x20);
    }
    CHECK(json::parse(line)["text"] == text);
}

TEST_CASE("json lines: invalid UTF-8 is replaced instead of throwing, output is pure ASCII") {
    std::string text = "caf";
    text += "\xC3\xA9";          // a valid e-acute
    text += "\xFF\xFE\xC3";      // invalid bytes
    text += "\xE2\x80\xA8";      // U+2028 line separator
    text += "\xF0\x9F\x98\x80";  // 4-byte character
    const std::string line = JsonLineWriter::format({{"event", "x"}, {"text", text}});
    for (const char ch : line) {
        CHECK(static_cast<unsigned char>(ch) < 0x80);
    }
    const json parsed = json::parse(line);
    const std::string back = parsed["text"].get<std::string>();
    CHECK(back.find("caf\xC3\xA9") == 0);
    CHECK(back.find("\xEF\xBF\xBD") != std::string::npos);  // U+FFFD
    CHECK(back.find("\xE2\x80\xA8") != std::string::npos);  // survived as a character
}

TEST_CASE("json lines: every string value is redacted, nested ones too, keys are left alone") {
    curia::SecretRegistry registry;
    REQUIRE(registry.add(kKey));
    json event = {{"event", "done"},
                  {"text", "the key is " + kKey + "!"},
                  {"nested", {{"deep", json::array({"x", kKey, {{"k", "a" + kKey}}})}}},
                  {"number", 5}};
    const std::string line = JsonLineWriter::format(event, &registry);
    CHECK(line.find("0123456789abcdef") == std::string::npos);
    const json parsed = json::parse(line);
    CHECK(parsed["text"] == "the key is [redacted]!");
    CHECK(parsed["nested"]["deep"][1] == "[redacted]");
    CHECK(parsed["nested"]["deep"][2]["k"] == "a[redacted]");
    CHECK(parsed["number"] == 5);
}

TEST_CASE("json lines: a key-shaped token is redacted even when it was never registered") {
    curia::SecretRegistry registry;
    const std::string line =
        JsonLineWriter::format({{"event", "x"}, {"text", "see " + kKey}}, &registry);
    CHECK(line.find("FAKE") == std::string::npos);
}

TEST_CASE("json lines: lines from several threads never interleave") {
    std::ostringstream out;
    JsonLineWriter writer(out);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&writer, t] {
            for (int i = 0; i < 100; ++i) {
                writer.write(
                    {{"event", "n"}, {"thread", t}, {"i", i}, {"pad", std::string(200, 'p')}});
            }
        });
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    std::istringstream lines(out.str());
    std::string line;
    int count = 0;
    while (std::getline(lines, line)) {
        CHECK(json::parse(line).is_object());
        ++count;
    }
    CHECK(count == 400);
}
