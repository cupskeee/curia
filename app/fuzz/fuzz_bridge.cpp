// SPDX-License-Identifier: MIT
// libFuzzer entry point for the bridge core: bytes -> LineSplitter -> parseLine -> FrameAssembler.
//
// The input is a log in the fixture format of tests/fixtures/curia1/README.md: a line that starts
// with the marker gets a synthetic engine-style prefix (so that most mutations land on records
// instead of on the prefix), "!raw " and "!noise " lines are verbatim, "!truncate" truncates the
// log, anything else is a verbatim line. Seeds are therefore plain bare-record files.
//
// Checked on every input (a failure aborts, which the fuzzer reports as a crash):
//  - nothing crashes or trips a sanitizer;
//  - the events do not depend on how the bytes are chunked (a hash of the input picks the chunks);
//  - a frame event (Complete or Damaged) is only ever emitted for a frame that a valid BEGIN
//    opened and that has no frame event yet, so there is at most one Damaged per frame, and the
//    assembler is Open exactly while a frame is pending;
//  - every Complete frame obeys the ranges and counts of docs/wire-format.md sections 2 to 4 and
//    holds clean, valid UTF-8 text;
//  - two small reference models, written from the spec and not from the code under test, agree
//    with it: a plain split of the bytes into lines (section 1: '\n', one '\r', the 4,096-byte
//    head), and a recount of every frame's records, snaps, byte total, END claim and shortened
//    flag at the moment it is reported Complete. Chunk independence cannot see a consistently
//    wrong rule; these can.
//
// The same function is linked into curia_fuzz_replay (fuzz_replay_main.cpp), which feeds it files
// without libFuzzer so that every platform can replay the seed corpus.
#include "fuzz_bridge.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "bridge/bridge_pipeline.h"
#include "bridge/frame_assembler.h"
#include "bridge/frame_json.h"
#include "bridge/line_splitter.h"
#include "bridge/record.h"

namespace {

using namespace curia::bridge;

constexpr std::size_t kMaxInputBytes = 1 << 20;
// The numbers of the spec, restated so that the reference models do not borrow them from the code
// under test: section 1 (line head), section 3 (frame limits).
constexpr std::size_t kSpecLineHead = 4096;
constexpr std::int64_t kSpecMaxFrameRecords = 512;
constexpr std::size_t kSpecMaxFrameBytes = 128 * 1024;

void require(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "fuzz_bridge: invariant violated: %s\n", what);
        std::abort();
    }
}

struct Step {
    bool truncate = false;
    std::string bytes;
};

// The synthetic prefix: 119 to 131 bytes without the marker (the shape seen in the game).
std::string prefix(std::size_t variant) {
    std::string out = "synthetic-time synthetic-level synthetic-source:1 synthetic/path.txt:1 ";
    out.append(118 + variant % 13 - out.size(), '.');
    out += ' ';
    return out;
}

std::vector<Step> toSteps(std::string_view input) {
    std::vector<Step> steps(1);
    std::size_t variant = 0;
    std::size_t pos = 0;
    while (pos < input.size()) {
        const std::size_t newline = input.find('\n', pos);
        const bool terminated = newline != std::string_view::npos;
        const std::string_view line =
            input.substr(pos, (terminated ? newline : input.size()) - pos);
        pos = terminated ? newline + 1 : input.size();

        std::string text;
        if (line == "!truncate") {
            steps.emplace_back();
            steps.back().truncate = true;
            steps.emplace_back();
            continue;
        }
        if (line.starts_with("CURIA1|")) {
            text = prefix(variant++);
            text.append(line);
        } else if (line.starts_with("!raw ")) {
            text.assign(line.substr(5));
        } else if (line.starts_with("!noise ")) {
            text.assign(line.substr(7));
        } else if (line != "!raw" && line != "!noise") {
            text.assign(line);
        }
        if (terminated) {
            text += '\n';
        }
        steps.back().bytes += text;
    }
    return steps;
}

std::uint64_t fnv1a(std::string_view data) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : data) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    return hash;
}

// Splits `bytes` into chunks of 1 to maxChunk bytes taken from a xorshift sequence.
template <typename Sink> void forChunks(std::string_view bytes, std::uint64_t seed, Sink&& sink) {
    std::uint64_t state = seed | 1U;
    const std::uint64_t maxChunk = 1 + (seed >> 20) % 300;
    std::size_t pos = 0;
    while (pos < bytes.size()) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        const auto want = static_cast<std::size_t>(1 + state % maxChunk);
        const std::size_t take = std::min(bytes.size() - pos, want);
        sink(bytes.substr(pos, take));
        pos += take;
    }
}

// Section 1 written the plain way: the physical lines of `bytes` that end in '\n' (a tail without
// one is held back), the head of an overlong line, one '\r' removed from a normal line.
std::vector<Line> referenceLines(std::string_view bytes) {
    std::vector<Line> lines;
    std::size_t pos = 0;
    while (true) {
        const std::size_t newline = bytes.find('\n', pos);
        if (newline == std::string_view::npos) {
            break;
        }
        std::string_view physical = bytes.substr(pos, newline - pos);
        Line line;
        if (physical.size() > kSpecLineHead) {
            line.overlong = true;
            physical = physical.substr(0, kSpecLineHead);
        } else if (!physical.empty() && physical.back() == '\r') {
            physical.remove_suffix(1);
        }
        line.text.assign(physical);
        lines.push_back(std::move(line));
        pos = newline + 1;
    }
    return lines;
}

std::string runPipeline(const std::vector<Step>& steps, bool chunked, std::uint64_t seed) {
    BridgePipeline pipeline;
    std::vector<Event> events;
    const auto take = [&events](std::vector<Event>&& more) {
        for (Event& e : more) {
            events.push_back(std::move(e));
        }
    };
    for (const Step& step : steps) {
        if (step.truncate) {
            take(pipeline.truncateLog());
        } else if (chunked) {
            forChunks(step.bytes, seed,
                      [&](std::string_view chunk) { take(pipeline.feed(chunk)); });
        } else {
            take(pipeline.feed(step.bytes));
        }
    }
    return dumpJson(toJson(events));
}

bool cleanText(const std::string& text) {
    // A record is cut to 1,024 bytes before hygiene, but hygiene step 1 turns each invalid byte
    // into U+FFFD (3 bytes), so a cleaned text can legitimately reach three times that.
    if (text.size() > 3 * kMaxRecordBytes) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
    }
    // Hygiene step 5: runs of spaces collapsed, no space at either end.
    if (!text.empty() &&
        (text.front() == ' ' || text.back() == ' ' || text.find("  ") != std::string::npos)) {
        return false;
    }
    // Valid UTF-8, no overlong forms, surrogates or values above U+10FFFF.
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t len = 1;
        std::uint32_t code = c;
        if (c >= 0xF0) {
            len = 4;
            code = c & 0x07U;
        } else if (c >= 0xE0) {
            len = 3;
            code = c & 0x0FU;
        } else if (c >= 0xC0) {
            len = 2;
            code = c & 0x1FU;
        } else if (c >= 0x80) {
            return false;
        }
        if (i + len > text.size()) {
            return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
            const auto cont = static_cast<unsigned char>(text[i + k]);
            if ((cont & 0xC0U) != 0x80U) {
                return false;
            }
            code = (code << 6) | (cont & 0x3FU);
        }
        // Hygiene step 4: C1 controls, line and paragraph separators, bidi controls.
        const bool deleted = (code >= 0x80 && code <= 0x9F) || code == 0x2028 || code == 0x2029 ||
                             (code >= 0x202A && code <= 0x202E) ||
                             (code >= 0x2066 && code <= 0x2069);
        if (deleted) {
            return false;
        }
        const std::uint32_t minimum = len == 1   ? 0U
                                      : len == 2 ? 0x80U
                                      : len == 3 ? 0x800U
                                                 : 0x10000U;
        if (code < minimum || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += len;
    }
    return true;
}

void checkCharacter(const std::optional<CharacterRef>& ref) {
    if (ref) {
        require(ref->id >= 0 && cleanText(ref->name), "faction character");
    }
}

void checkFrame(const Frame& f) {
    require(f.snap >= 1, "frame snap >= 1");
    require(f.mode == 0 || f.mode == 1, "mode is 0 or 1");
    require(f.date.year >= 0 && f.date.year <= 9999, "year range");
    require(f.date.month >= 1 && f.date.month <= 12, "month range");
    require(f.date.day >= 1 && f.date.day <= 31, "day range");
    require(f.rulerId >= 0, "ruler id");
    require(cleanText(f.rulerName), "ruler name text");
    require(!f.modVersion.empty() && f.modVersion.size() <= 32, "mod version token");
    require(f.recordCount >= 2 && f.recordCount <= 512, "record count 2..512");

    std::int64_t minimum = 2;
    if (f.fin) {
        ++minimum;
    }
    if (f.heir) {
        ++minimum;
        if (!f.heir->none) {
            require(f.heir->id >= 0, "heir id");
            require(cleanText(f.heir->name), "heir name text");
        }
    }
    if (f.vassals) {
        const VassalsSection& v = *f.vassals;
        minimum += 1 + static_cast<std::int64_t>(v.listed.size());
        require(v.total >= 0 && static_cast<std::int64_t>(v.listed.size()) <= v.total,
                "vassals listed <= total");
        std::int32_t rank = 0;
        for (const Vassal& vassal : v.listed) {
            require(vassal.rank == ++rank, "vassal ranks run 1..listed");
            require(vassal.id >= 0 && vassal.strength >= 0, "vassal ids and strength");
            require(cleanText(vassal.name), "vassal name text");
        }
    }
    if (f.wars) {
        const WarsSection& w = *f.wars;
        minimum += 1 + 3 * static_cast<std::int64_t>(w.wars.size());
        require(static_cast<std::int64_t>(w.wars.size()) == w.total, "wars total == groups");
        for (const War& war : w.wars) {
            require(cleanText(war.name) && cleanText(war.attacker.name) &&
                        cleanText(war.defender.name),
                    "war texts");
            require(war.attacker.id >= 0 && war.defender.id >= 0, "war side ids");
        }
    }
    if (f.claims) {
        const ClaimsSection& c = *f.claims;
        minimum += 1 + static_cast<std::int64_t>(c.listed.size());
        require(c.explicitCount >= 0 && c.implicitCount >= 0, "claim totals");
        require(static_cast<std::int64_t>(c.listed.size()) <= c.explicitCount,
                "claims listed <= explicit");
        for (const Claim& claim : c.listed) {
            require(claim.tier >= 1 && claim.tier <= 5, "claim tier");
            require(cleanText(claim.title), "claim title text");
        }
    }
    if (f.factions) {
        const FactionsSection& s = *f.factions;
        minimum += 1 + 5 * static_cast<std::int64_t>(s.listed.size());
        std::int32_t members = 0;
        std::int32_t against = 0;
        for (const Faction& faction : s.listed) {
            (faction.role == FactionRole::Member ? members : against) += 1;
            require(faction.type >= 1 && faction.type <= 7, "faction type");
            require(faction.power >= 0 && faction.threshold >= 0 && faction.members >= 0,
                    "faction numbers");
            if (faction.type != 3) {
                require(!faction.claimant && !faction.title, "claimant and title only for type 3");
            }
            checkCharacter(faction.leader);
            checkCharacter(faction.claimant);
            checkCharacter(faction.target);
            if (faction.title) {
                require(faction.title->tier >= 1 && faction.title->tier <= 5, "faction title tier");
                require(cleanText(faction.title->title), "faction title text");
            }
        }
        require(members == (s.member ? 1 : 0), "faction groups with role member == member");
        require(against <= s.against, "faction groups against <= against");
    }
    require(f.recordCount >= minimum, "record count covers the records present");
    // Printing must never throw or crash.
    require(!dumpJson(toJson(f)).empty(), "frame prints");
}

// What the reference model knows about the frame a valid BEGIN opened, counted from the parse
// results alone.
struct FrameModel {
    std::int64_t count = 0;  // frame records so far, BEGIN and (once it came) END included
    std::size_t bytes = 0;
    std::int32_t snap = 0;
    bool snapMismatch = false;
    bool shortened = false;
    std::int64_t endClaim = -1;
};

// The manual chain: the same stages as BridgePipeline, but with the frame bookkeeping visible.
void checkInvariants(const std::vector<Step>& steps, std::uint64_t seed) {
    LineSplitter splitter;
    FrameAssembler assembler;
    bool pending = false;  // a valid BEGIN opened a frame that has had no frame event yet
    FrameModel model;
    std::vector<Line> lines;  // what the splitter produced for the current step

    const auto handle = [&](const std::vector<Event>& events) {
        for (const Event& event : events) {
            if (const auto* complete = std::get_if<FrameComplete>(&event)) {
                require(pending, "Complete only for a pending frame");
                pending = false;
                checkFrame(complete->frame);
                const Frame& f = complete->frame;
                require(f.recordCount == model.count, "record count equals the recount");
                require(model.endClaim == model.count, "END claimed the recounted number");
                require(!model.snapMismatch && f.snap == model.snap, "all records share the snap");
                require(model.count <= kSpecMaxFrameRecords && model.bytes <= kSpecMaxFrameBytes,
                        "a complete frame is within the size limits");
                require(f.textShortened == model.shortened, "text shortened flag");
            } else if (const auto* damaged = std::get_if<FrameDamaged>(&event)) {
                require(pending, "at most one Damaged per frame, and only for a pending frame");
                pending = false;
                require(damaged->snap >= 1, "damaged snap >= 1");
            } else if (std::holds_alternative<UnsupportedFormat>(event)) {
                require(!pending, "an unsupported-format report leaves no frame pending");
            }
        }
    };
    const auto consumeLine = [&](Line&& line) {
        lines.push_back(line);
        const ParseResult result = parseLine(line.text, line.overlong);
        const bool isFrameRecord =
            result.outcome == ParseOutcome::Record && result.record.snap >= 1;
        const bool opens = isFrameRecord && result.record.type == RecordType::Begin;
        if (isFrameRecord && !opens && pending) {
            const Record& record = result.record;
            ++model.count;
            model.bytes += record.length;
            model.snapMismatch = model.snapMismatch || record.snap != model.snap;
            if (record.type == RecordType::End) {
                const auto* end = std::get_if<EndFields>(&record.fields);
                model.endClaim = end != nullptr ? end->count : -1;
            } else if (record.textShortened) {
                model.shortened = true;
            }
        }
        handle(assembler.consume(result));
        if (opens) {
            pending = true;
            model = FrameModel{};
            model.count = 1;
            model.bytes = result.record.length;
            model.snap = result.record.snap;
            model.shortened = result.record.textShortened;
        }
        require((assembler.state() == FrameAssembler::State::Open) == pending,
                "assembler is Open exactly while a frame is pending");
    };
    const auto checkLines = [&](std::string_view stepBytes) {
        const std::vector<Line> expected = referenceLines(stepBytes);
        require(expected.size() == lines.size(), "line count equals the reference split");
        for (std::size_t i = 0; i < expected.size(); ++i) {
            require(expected[i].text == lines[i].text && expected[i].overlong == lines[i].overlong,
                    "line equals the reference split");
        }
        lines.clear();
    };
    for (const Step& step : steps) {
        if (step.truncate) {
            splitter.reset();
            handle(assembler.truncateLog());
            pending = false;
            require(assembler.state() != FrameAssembler::State::Open, "truncation closes a frame");
            continue;
        }
        forChunks(step.bytes, seed,
                  [&](std::string_view chunk) { splitter.feed(chunk, consumeLine); });
        checkLines(step.bytes);
    }
}

}  // namespace

std::string fuzzEventsJson(std::string_view input) {
    return runPipeline(toSteps(input), false, 0);
}

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size > kMaxInputBytes) {
        return 0;
    }
    const std::string_view input(reinterpret_cast<const char*>(data), size);
    const std::vector<Step> steps = toSteps(input);
    const std::uint64_t seed = fnv1a(input);

    require(runPipeline(steps, true, seed) == runPipeline(steps, false, seed),
            "events do not depend on the chunking");
    checkInvariants(steps, seed);
    return 0;
}
