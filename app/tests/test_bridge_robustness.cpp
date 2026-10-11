// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "bridge/bridge_pipeline.h"
#include "bridge/frame_json.h"
#include "bridge/line_splitter.h"
#include "bridge/record.h"
#include "bridge/text_hygiene.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using namespace curia::bridge::test;

namespace {

// xorshift32: the same sequence on every platform and standard library.
class Rng {
public:
    explicit Rng(std::uint32_t seed) : state_(seed == 0 ? 0x9E3779B9U : seed) {}
    std::uint32_t next() {
        state_ ^= state_ << 13;
        state_ ^= state_ >> 17;
        state_ ^= state_ << 5;
        return state_;
    }
    std::size_t below(std::size_t n) { return n == 0 ? 0 : next() % n; }

private:
    std::uint32_t state_;
};

std::vector<std::string> splitLines(const std::string& s) {
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos < s.size()) {
        const std::size_t nl = s.find('\n', pos);
        if (nl == std::string::npos) {
            lines.push_back(s.substr(pos));
            break;
        }
        lines.push_back(s.substr(pos, nl - pos + 1));
        pos = nl + 1;
    }
    return lines;
}

std::string joinLines(const std::vector<std::string>& lines) {
    std::string out;
    for (const std::string& line : lines) {
        out += line;
    }
    return out;
}

const std::vector<std::string>& interestingBytes() {
    static const std::vector<std::string> v = {
        "|",
        "\n",
        "\r",
        "\r\n",
        "CURIA1|",
        "0",
        "-",
        "-0",
        "9999999999",
        "\x15",
        "\x15!",
        "\xFF",
        std::string(1, '\0'),
        "\xE2\x82",
        "none",
        "BEGIN",
        "END",
        "FACTION",
        "WAR",
        "|none|",
        "weak",
        "against",
        " ",
        "007",
    };
    return v;
}

std::string mutate(std::string s, Rng& rng) {
    const std::size_t rounds = 1 + rng.below(4);
    for (std::size_t round = 0; round < rounds; ++round) {
        switch (rng.below(9)) {
        case 0:  // flip a byte
            if (!s.empty()) {
                s[rng.below(s.size())] = static_cast<char>(rng.next() & 0xFFU);
            }
            break;
        case 1:  // delete a range
            if (!s.empty()) {
                const std::size_t at = rng.below(s.size());
                s.erase(at, rng.below(24));
            }
            break;
        case 2:  // insert something interesting
            s.insert(rng.below(s.size() + 1),
                     interestingBytes()[rng.below(interestingBytes().size())]);
            break;
        case 3:  // truncate anywhere (leaves an unterminated tail)
            s.resize(rng.below(s.size() + 1));
            break;
        case 4: {  // duplicate a line
            auto lines = splitLines(s);
            if (!lines.empty()) {
                const std::size_t at = rng.below(lines.size());
                lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(at), lines[at]);
                s = joinLines(lines);
            }
            break;
        }
        case 5: {  // delete a line
            auto lines = splitLines(s);
            if (!lines.empty()) {
                lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(rng.below(lines.size())));
                s = joinLines(lines);
            }
            break;
        }
        case 6: {  // swap two lines
            auto lines = splitLines(s);
            if (lines.size() > 1) {
                std::swap(lines[rng.below(lines.size())], lines[rng.below(lines.size())]);
                s = joinLines(lines);
            }
            break;
        }
        case 7: {  // change a digit
            const std::size_t at = s.find_first_of("0123456789", rng.below(s.size() + 1));
            if (at != std::string::npos) {
                s[at] = static_cast<char>('0' + rng.below(10));
            }
            break;
        }
        default: {  // a very long line
            s.insert(rng.below(s.size() + 1), std::string(1 + rng.below(6000), 'z'));
            break;
        }
        }
    }
    return s;
}

bool validText(const std::string& s) {
    if (repairUtf8(s) != s) {
        return false;
    }
    if (!s.empty() && (s.front() == ' ' || s.back() == ' ')) {
        return false;
    }
    if (s.find("  ") != std::string::npos) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        const auto b = static_cast<unsigned char>(c);
        return b >= 0x20 && b != 0x7F;
    });
}

void checkFrame(const Frame& f) {
    CHECK(f.snap >= 1);
    CHECK((f.mode == 0 || f.mode == 1));
    CHECK(f.date.month >= 1);
    CHECK(f.date.month <= 12);
    CHECK(f.date.day >= 1);
    CHECK(f.date.day <= 31);
    CHECK(f.date.year >= 0);
    CHECK(f.date.year <= 9999);
    CHECK(validText(f.rulerName));
    CHECK(f.recordCount >= 2);
    CHECK(f.recordCount <= 512);
    std::int64_t minimum = 2;  // BEGIN and END
    if (f.fin) {
        minimum += 1;
    }
    if (f.heir) {
        minimum += 1;
        CHECK(validText(must(f.heir).name));
    }
    if (f.vassals) {
        minimum += 1 + static_cast<std::int64_t>(must(f.vassals).listed.size());
        CHECK(static_cast<std::int64_t>(must(f.vassals).listed.size()) <= must(f.vassals).total);
        std::int32_t rank = 1;
        for (const Vassal& v : must(f.vassals).listed) {
            CHECK(v.rank == rank++);
            CHECK(validText(v.name));
        }
    }
    if (f.wars) {
        minimum += 1 + 3 * static_cast<std::int64_t>(must(f.wars).wars.size());
        CHECK(static_cast<std::int64_t>(must(f.wars).wars.size()) == must(f.wars).total);
        for (const War& w : must(f.wars).wars) {
            CHECK(validText(w.name));
            CHECK(validText(w.attacker.name));
            CHECK(validText(w.defender.name));
        }
    }
    if (f.claims) {
        minimum += 1 + static_cast<std::int64_t>(must(f.claims).listed.size());
        CHECK(static_cast<std::int64_t>(must(f.claims).listed.size()) <=
              must(f.claims).explicitCount);
        for (const Claim& c : must(f.claims).listed) {
            CHECK(c.tier >= 1);
            CHECK(c.tier <= 5);
            CHECK(validText(c.title));
        }
    }
    if (f.factions) {
        minimum += 1 + 5 * static_cast<std::int64_t>(must(f.factions).listed.size());
        std::int64_t members = 0;
        std::int64_t against = 0;
        for (const Faction& g : must(f.factions).listed) {
            (g.role == FactionRole::Member ? members : against) += 1;
            CHECK(g.type >= 1);
            CHECK(g.type <= 7);
            if (g.leader) {
                CHECK(validText(must(g.leader).name));
            }
            if (g.claimant) {
                CHECK(validText(must(g.claimant).name));
            }
            if (g.title) {
                CHECK(validText(must(g.title).title));
            }
            if (g.target) {
                CHECK(validText(must(g.target).name));
            }
        }
        CHECK(members == (must(f.factions).member ? 1 : 0));
        CHECK(against <= must(f.factions).against);
    }
    // The END count equals the frame records: sections and items account for part of them, unknown
    // records for the rest.
    CHECK(static_cast<std::int64_t>(f.recordCount) >= minimum);
}

// Feeds `bytes` line by line and checks the invariants against an independent view of the lines.
void checkInvariants(const std::string& bytes) {
    LineSplitter splitter;
    const std::vector<Line> lines = splitter.feed(bytes);
    BridgePipeline pipeline;
    bool needBegin = true;
    std::int32_t openSnap = 0;
    std::int64_t frameRecords = 0;
    std::int64_t terminalEvents = 0;
    std::int64_t validBegins = 0;
    std::int64_t validEnds = 0;
    std::int64_t completes = 0;
    for (const Line& line : lines) {
        const ParseResult parsed = parseLine(line.text, line.overlong);
        const bool isFrameRecord =
            parsed.outcome == ParseOutcome::Record && parsed.record.snap >= 1;
        const bool isBegin = isFrameRecord && parsed.record.type == RecordType::Begin;
        if (isFrameRecord && parsed.record.type == RecordType::End) {
            ++validEnds;
        }
        if (isFrameRecord) {
            ++frameRecords;
        }
        const std::vector<Event> events = pipeline.feedLine(line);
        for (const Event& event : events) {
            if (const auto* c = std::get_if<FrameComplete>(&event)) {
                CHECK_FALSE(needBegin);  // at most one terminal event per frame
                CHECK(c->frame.snap == openSnap);
                CHECK(c->frame.recordCount == frameRecords);
                checkFrame(c->frame);
                needBegin = true;
                ++terminalEvents;
                ++completes;
            } else if (const auto* d = std::get_if<FrameDamaged>(&event)) {
                CHECK_FALSE(needBegin);
                CHECK(d->snap == openSnap);
                CHECK(d->snap >= 1);
                CHECK_FALSE(d->detail.empty());
                needBegin = true;
                ++terminalEvents;
            } else if (const auto* u = std::get_if<UnsupportedFormat>(&event)) {
                CHECK(u->snap >= 1);
                CHECK(u->leadingFields.size() <= 64);
            }
        }
        if (isBegin) {
            needBegin = false;
            openSnap = parsed.record.snap;
            frameRecords = 1;
            ++validBegins;
        }
    }
    CHECK(terminalEvents <= validBegins);
    CHECK(completes <= validEnds);
}

}  // namespace

TEST_CASE("robustness: mutated fixtures never break the invariants") {
    Rng rng(20261011);
    std::vector<std::string> corpus;
    for (const std::string& name : fixtureNames()) {
        std::string bytes;
        for (const Action& action : loadFixture(name).actions) {
            bytes += action.bytes;  // truncations are dropped: the stream is one log
        }
        corpus.push_back(std::move(bytes));
    }
    REQUIRE_FALSE(corpus.empty());
    for (const std::string& original : corpus) {
        checkInvariants(original);
        for (int i = 0; i < 25; ++i) {
            const std::string mutated = mutate(original, rng);
            // The same bytes in any chunking give the same events.
            const std::string whole = eventsText(runBytes(mutated));
            BridgePipeline chunked;
            std::vector<Event> events;
            std::size_t pos = 0;
            while (pos < mutated.size()) {
                const std::size_t take =
                    std::min<std::size_t>(1 + rng.below(200), mutated.size() - pos);
                for (Event& e : chunked.feed(std::string_view(mutated).substr(pos, take))) {
                    events.push_back(std::move(e));
                }
                pos += take;
            }
            CHECK(eventsText(events) == whole);
            checkInvariants(mutated);
        }
    }
}

TEST_CASE("robustness: concatenated and repeated fixtures") {
    Rng rng(77);
    std::string all;
    for (const std::string& name : fixtureNames()) {
        for (const Action& action : loadFixture(name).actions) {
            all += action.bytes;
        }
    }
    checkInvariants(all);
    checkInvariants(all + all);
    // A shuffle of whole lines.
    auto lines = splitLines(all);
    for (std::size_t i = lines.size(); i > 1; --i) {
        std::swap(lines[i - 1], lines[rng.below(i)]);
    }
    checkInvariants(joinLines(lines));
}

TEST_CASE("robustness: random byte soup with record fragments") {
    Rng rng(4242);
    const std::vector<std::string> pieces = {
        "CURIA1|", "7|",       "BEGIN|", "END|",       "FIN|",    "HEIR|", "VASSAL|", "FACTION|",
        "WAR|",    "none",     "|",      "0",          "1",       "-1",    "12",      "\n",
        "\r\n",    " ",        "x",      "weak",       "against", "INFO",  "NAME",    "LEADER",
        "\x15",    "\xC3\xA9", "\xFF",   "2147483647", "-0",
    };
    for (int run = 0; run < 400; ++run) {
        std::string s;
        const std::size_t n = rng.below(120);
        for (std::size_t i = 0; i < n; ++i) {
            s += pieces[rng.below(pieces.size())];
        }
        checkInvariants(s);
    }
}

TEST_CASE("robustness: the parser survives every prefix and byte-flip of every fixture line") {
    Rng rng(99);
    for (const std::string& name : fixtureNames()) {
        for (const Action& action : loadFixture(name).actions) {
            for (const Line& line : LineSplitter().feed(action.bytes)) {
                for (std::size_t n = 0; n <= std::min<std::size_t>(line.text.size(), 400); ++n) {
                    (void)parseLine(std::string_view(line.text).substr(0, n), line.overlong);
                }
                std::string copy = line.text;
                for (int k = 0; k < 8 && !copy.empty(); ++k) {
                    copy[rng.below(copy.size())] = static_cast<char>(rng.next() & 0xFFU);
                    (void)parseLine(copy, false);
                }
            }
        }
    }
    CHECK(true);  // reaching this line without a crash is the check
}
