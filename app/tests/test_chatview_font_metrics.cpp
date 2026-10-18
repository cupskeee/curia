// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "chatview/font_metrics.h"

using curia::chatview::FontVerticalMetrics;
using curia::chatview::readVerticalMetrics;

namespace {

struct Table {
    std::string tag;
    std::vector<unsigned char> bytes;
};

void put16(std::vector<unsigned char>& out, std::size_t at, std::uint32_t value) {
    out[at] = static_cast<unsigned char>((value >> 8U) & 0xFFU);
    out[at + 1] = static_cast<unsigned char>(value & 0xFFU);
}

void put32(std::vector<unsigned char>& out, std::size_t at, std::uint32_t value) {
    put16(out, at, value >> 16U);
    put16(out, at + 2, value & 0xFFFFU);
}

std::vector<unsigned char> headTable(std::uint32_t unitsPerEm) {
    std::vector<unsigned char> head(54, 0);
    put16(head, 18, unitsPerEm);
    return head;
}

std::vector<unsigned char> hheaTable(int ascender, int descender) {
    std::vector<unsigned char> hhea(36, 0);
    put16(hhea, 4, static_cast<std::uint16_t>(static_cast<std::int16_t>(ascender)));
    put16(hhea, 6, static_cast<std::uint16_t>(static_cast<std::int16_t>(descender)));
    return hhea;
}

// A font file with these tables: the offset table and all records first, then the tables in order.
std::vector<unsigned char> fontFile(const std::vector<Table>& tables,
                                    const std::string& version = std::string("\0\1\0\0", 4)) {
    std::vector<unsigned char> out(12 + 16 * tables.size(), 0);
    for (std::size_t i = 0; i < 4; ++i) {
        out[i] = static_cast<unsigned char>(version[i]);
    }
    put16(out, 4, static_cast<std::uint32_t>(tables.size()));
    for (std::size_t i = 0; i < tables.size(); ++i) {
        const std::size_t record = 12 + 16 * i;
        for (std::size_t k = 0; k < 4; ++k) {
            out[record + k] = static_cast<unsigned char>(tables[i].tag[k]);
        }
        put32(out, record + 8, static_cast<std::uint32_t>(out.size()));
        put32(out, record + 12, static_cast<std::uint32_t>(tables[i].bytes.size()));
        out.insert(out.end(), tables[i].bytes.begin(), tables[i].bytes.end());
    }
    return out;
}

std::optional<FontVerticalMetrics> read(const std::vector<unsigned char>& bytes) {
    return readVerticalMetrics(bytes.data(), bytes.size());
}

}  // namespace

TEST_CASE("font metrics: the bundled face's numbers") {
    // Gentium Book Plus 6.200: 2048 units per em, hhea ascender 2250, descender -750.
    const auto bytes = fontFile({{"hhea", hheaTable(2250, -750)}, {"head", headTable(2048)}});
    const std::optional<FontVerticalMetrics> metrics = read(bytes);
    REQUIRE(metrics.has_value());
    if (metrics.has_value()) {
        CHECK(metrics->unitsPerEm == 2048);
        CHECK(metrics->ascender == 2250);
        CHECK(metrics->descender == -750);
        CHECK(metrics->heightPerEm() == doctest::Approx(3000.0 / 2048.0));
    }
}

TEST_CASE("font metrics: the order of the tables and other tables do not matter") {
    const auto bytes = fontFile({{"cmap", std::vector<unsigned char>(20, 7)},
                                 {"head", headTable(1000)},
                                 {"OS/2", std::vector<unsigned char>(96, 1)},
                                 {"hhea", hheaTable(800, -200)}});
    const std::optional<FontVerticalMetrics> metrics = read(bytes);
    REQUIRE(metrics.has_value());
    if (metrics.has_value()) {
        CHECK(metrics->heightPerEm() == doctest::Approx(1.0));
    }
    // An OpenType file with CFF outlines has the same tables.
    CHECK(read(fontFile({{"head", headTable(1000)}, {"hhea", hheaTable(800, -200)}}, "OTTO"))
              .has_value());
}

TEST_CASE("font metrics: what is refused") {
    const auto good = fontFile({{"head", headTable(2048)}, {"hhea", hheaTable(2250, -750)}});
    CHECK(read(good).has_value());
    CHECK_FALSE(readVerticalMetrics(nullptr, 100).has_value());
    CHECK_FALSE(readVerticalMetrics(good.data(), 0).has_value());
    CHECK_FALSE(readVerticalMetrics(good.data(), 11).has_value());
    // Cut anywhere before the end of the last table.
    for (std::size_t size = 0; size < good.size(); ++size) {
        CAPTURE(size);
        CHECK_FALSE(readVerticalMetrics(good.data(), size).has_value());
    }
    // A collection, a missing table, impossible values.
    CHECK_FALSE(read(fontFile({{"head", headTable(2048)}, {"hhea", hheaTable(2250, -750)}}, "ttcf"))
                    .has_value());
    CHECK_FALSE(read(fontFile({{"head", headTable(2048)}})).has_value());
    CHECK_FALSE(read(fontFile({{"hhea", hheaTable(2250, -750)}})).has_value());
    CHECK_FALSE(
        read(fontFile({{"head", headTable(0)}, {"hhea", hheaTable(2250, -750)}})).has_value());
    CHECK_FALSE(
        read(fontFile({{"head", headTable(65535)}, {"hhea", hheaTable(2250, -750)}})).has_value());
    CHECK_FALSE(
        read(fontFile({{"head", headTable(2048)}, {"hhea", hheaTable(-10, -10)}})).has_value());
    // A table shorter than the fields read from it.
    CHECK_FALSE(read(fontFile({{"head", std::vector<unsigned char>(19, 0)},
                               {"hhea", hheaTable(2250, -750)}}))
                    .has_value());
    // A record that points past the end.
    auto broken = good;
    put32(broken, 12 + 8, 0xFFFFFFF0U);
    CHECK_FALSE(read(broken).has_value());
    // More records than the file holds.
    auto tooMany = good;
    put16(tooMany, 4, 4000);
    CHECK_FALSE(read(tooMany).has_value());
}
