// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cstdint>
#include <initializer_list>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "bridge/bridge_pipeline.h"
#include "bridge/frame_assembler.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using curia::bridge::test::eventsOfType;
using curia::bridge::test::exampleFrame;
using curia::bridge::test::must;
using curia::bridge::test::onlyEvent;
using curia::bridge::test::prefixedLine;

namespace {

// Feeds bare records (one physical line each, with a synthetic engine prefix).
class Rig {
public:
    std::vector<Event> feed(const std::vector<std::string>& bareRecords) {
        std::vector<Event> all;
        for (const std::string& record : bareRecords) {
            for (Event& e : pipeline_.feed(prefixedLine(record, counter_++))) {
                all.push_back(std::move(e));
            }
        }
        return all;
    }
    std::vector<Event> feed(const std::string& bareRecord) {
        return feed(std::vector<std::string>{bareRecord});
    }
    std::vector<Event> truncate() { return pipeline_.truncateLog(); }
    [[nodiscard]] FrameAssembler::State state() const { return pipeline_.assembler().state(); }

private:
    BridgePipeline pipeline_;
    std::size_t counter_ = 0;
};

std::string r(std::int32_t snap, const std::string& rest) {
    return "CURIA1|" + std::to_string(snap) + "|" + rest;
}

std::string begin(std::int32_t snap) {
    return r(snap, "BEGIN|0|0.1.0|1066|9|15|1001|Test Ruler One");
}

std::string end(std::int32_t snap, std::size_t count) {
    return r(snap, "END|" + std::to_string(count));
}

// BEGIN + extra records + END with the right count.
std::vector<std::string> frameOf(std::int32_t snap, std::vector<std::string> middle) {
    std::vector<std::string> out;
    out.push_back(begin(snap));
    for (std::string& m : middle) {
        out.push_back(std::move(m));
    }
    out.push_back(end(snap, out.size() + 1));
    return out;
}

const FrameDamaged* damagedOnly(const std::vector<Event>& events) {
    return onlyEvent<FrameDamaged>(events);
}

const Frame* completeOnly(const std::vector<Event>& events) {
    const auto* c = onlyEvent<FrameComplete>(events);
    return c != nullptr ? &c->frame : nullptr;
}

bool damagedWith(const std::vector<Event>& events, DamageReason reason) {
    const auto* d = damagedOnly(events);
    return d != nullptr && d->reason == reason;
}

// A frame whose middle breaks one rule; the result is the single event of the frame.
std::vector<Event> run(const std::vector<std::string>& middle, std::int32_t snap = 5) {
    Rig rig;
    return rig.feed(frameOf(snap, middle));
}

}  // namespace

// ---- complete frames
// -----------------------------------------------------------------------------

TEST_CASE("assembler: the example frame is Complete with typed contents") {
    Rig rig;
    const auto events = rig.feed(exampleFrame(7));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->snap == 7);
    CHECK(frame->mode == 0);
    CHECK(frame->modVersion == "0.1.0");
    CHECK(frame->date.year == 1066);
    CHECK(frame->date.month == 9);
    CHECK(frame->date.day == 15);
    CHECK(frame->rulerId == 1001);
    CHECK(frame->rulerName == "Test Ruler One");
    CHECK(frame->recordCount == 16);
    CHECK_FALSE(frame->textShortened);
    CHECK(rig.state() == FrameAssembler::State::Idle);

    REQUIRE(frame->fin.has_value());
    CHECK(must(frame->fin).gold == 312);
    CHECK(must(frame->fin).incomeTenths == 78);

    REQUIRE(frame->heir.has_value());
    CHECK_FALSE(must(frame->heir).none);
    CHECK(must(frame->heir).id == 1002);
    CHECK(must(frame->heir).opinion == 84);
    CHECK(must(frame->heir).hasLiege);
    CHECK_FALSE(must(frame->heir).landed);
    CHECK(must(frame->heir).name == "Test Heir Two");

    REQUIRE(frame->vassals.has_value());
    CHECK(must(frame->vassals).total == 2);
    REQUIRE(must(frame->vassals).listed.size() == 2);
    CHECK(must(frame->vassals).listed[0].rank == 1);
    CHECK(must(frame->vassals).listed[0].name == "Test Vassal Three");
    CHECK(must(frame->vassals).listed[1].opinion == -12);
    CHECK(must(frame->vassals).listed[1].strength == 310);

    REQUIRE(frame->wars.has_value());
    CHECK(must(frame->wars).total == 0);
    CHECK(must(frame->wars).wars.empty());

    REQUIRE(frame->claims.has_value());
    CHECK(must(frame->claims).explicitCount == 1);
    CHECK(must(frame->claims).implicitCount == 0);
    REQUIRE(must(frame->claims).listed.size() == 1);
    CHECK(must(frame->claims).listed[0].kind == ClaimKind::Weak);
    CHECK(must(frame->claims).listed[0].tier == 4);
    CHECK(must(frame->claims).listed[0].title == "Test Realm");

    REQUIRE(frame->factions.has_value());
    CHECK(must(frame->factions).against == 1);
    CHECK_FALSE(must(frame->factions).member);
    REQUIRE(must(frame->factions).listed.size() == 1);
    const Faction& f = must(frame->factions).listed[0];
    CHECK(f.role == FactionRole::Against);
    CHECK(f.type == 3);
    CHECK(f.power == 33);
    CHECK(f.threshold == 100);
    CHECK(f.members == 2);
    REQUIRE(f.leader.has_value());
    CHECK(must(f.leader).id == 1004);
    CHECK(must(f.leader).name == "Test Vassal Four");
    REQUIRE(f.claimant.has_value());
    CHECK(must(f.claimant).id == 1005);
    REQUIRE(f.title.has_value());
    CHECK(must(f.title).tier == 4);
    CHECK(must(f.title).title == "Test Realm");
    REQUIRE(f.target.has_value());
    CHECK(must(f.target).id == 1001);
}

TEST_CASE("assembler: a minimal frame has no sections at all (absent means not exported)") {
    Rig rig;
    const auto events = rig.feed(frameOf(1, {}));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK_FALSE(frame->fin.has_value());
    CHECK_FALSE(frame->heir.has_value());
    CHECK_FALSE(frame->vassals.has_value());
    CHECK_FALSE(frame->wars.has_value());
    CHECK_FALSE(frame->claims.has_value());
    CHECK_FALSE(frame->factions.has_value());
    CHECK(frame->recordCount == 2);
}

TEST_CASE("assembler: empty sections are present and empty") {
    const auto events = run({r(5, "HEIR|none"), r(5, "VASSALS|0|0"), r(5, "WARS|0"),
                             r(5, "CLAIMS|0|0|0"), r(5, "FACTIONS|0|0|0")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->heir.has_value());
    CHECK(must(frame->heir).none);
    REQUIRE(frame->vassals.has_value());
    CHECK(must(frame->vassals).listed.empty());
    REQUIRE(frame->wars.has_value());
    REQUIRE(frame->claims.has_value());
    REQUIRE(frame->factions.has_value());
    CHECK(must(frame->factions).listed.empty());
}

TEST_CASE("assembler: hygiene is applied when text is copied into the frame") {
    const std::string u(1, '\x15');
    const std::string messy = "Install " + u + "ONCLICK:CHARACTER,1 " + u + "TOOLTIP:CHARACTER,1 " +
                              u + "L Test Ruler" + u + "!" + u + "!" + u + "! on the " + u +
                              "ONCLICK:TITLE,2 " + u + "TOOLTIP:LANDED_TITLE,2 " + u +
                              "L; Test Realm" + u + "!" + u + "!" + u + "! Throne";
    const auto events = run({r(5, "VASSALS|1|1"), r(5, "VASSAL|1|2|3|4|5|" + messy)});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->vassals.has_value());
    REQUIRE(must(frame->vassals).listed.size() == 1);
    CHECK(must(frame->vassals).listed[0].name == "Install Test Ruler on the Test Realm Throne");
}

TEST_CASE("assembler: a name with a pipe stays whole") {
    const auto events = run({r(5, "HEIR|1|2|1|0|Test | Heir | Two")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->heir.has_value());
    CHECK(must(frame->heir).name == "Test | Heir | Two");
}

TEST_CASE("assembler: non-Curia lines inside a frame are ignored and not counted") {
    BridgePipeline pipeline;
    std::string bytes;
    const auto frame = exampleFrame(7);
    for (std::size_t i = 0; i < frame.size(); ++i) {
        bytes += prefixedLine(frame[i], i);
        bytes += "some engine noise without the marker\n";
        bytes += "CURIA2|7|FIN|1|2 another marker version\n";
    }
    const auto events = pipeline.feed(bytes);
    REQUIRE(completeOnly(events) != nullptr);
}

// ---- the state machine, row by row
// --------------------------------------------------------------- Every TEST_CASE named "state row:
// ..." is one cell of the table in docs/wire-format.md section 3 (rows: valid BEGIN, invalid BEGIN,
// other known type, unknown type, invalid record, snap 0, END, the 513th record or 128 KiB,
// truncation; columns: Idle, Open, Damaged).

TEST_CASE("state row: valid BEGIN in Idle opens a frame; a shortened ruler name sets the flag") {
    Rig rig;
    CHECK(rig.feed(begin(3)).empty());
    CHECK(rig.state() == FrameAssembler::State::Open);
    const auto events = rig.feed(end(3, 2));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK_FALSE(frame->textShortened);

    // A ruler name over 1024 bytes: shortened, the frame stays Complete and carries the flag.
    const auto longName =
        rig.feed({r(4, "BEGIN|0|0.1.0|1066|9|15|1001|" + std::string(1500, 'r')), end(4, 2)});
    const Frame* shortened = completeOnly(longName);
    REQUIRE(shortened != nullptr);
    CHECK(shortened->textShortened);
    CHECK(shortened->rulerId == 1001);
    CHECK(shortened->rulerName.size() < 1024);
}

TEST_CASE("state row: valid BEGIN in Open abandons the frame as incomplete and opens a new one") {
    Rig rig;
    rig.feed({begin(3), r(3, "FIN|1|2")});
    const auto events = rig.feed(begin(4));
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::Incomplete);
    CHECK(d->snap == 3);
    CHECK(rig.state() == FrameAssembler::State::Open);
    const auto done = rig.feed(end(4, 2));
    const Frame* frame = completeOnly(done);
    REQUIRE(frame != nullptr);
    CHECK(frame->snap == 4);
    CHECK_FALSE(frame->fin.has_value());  // nothing of the abandoned frame leaked in
}

TEST_CASE("state row: valid BEGIN in Damaged gives no event and opens a new frame") {
    Rig rig;
    rig.feed({begin(3)});
    CHECK(damagedWith(rig.feed(r(3, "FIN|bad|1")), DamageReason::InvalidRecord));
    CHECK(rig.state() == FrameAssembler::State::Damaged);
    CHECK(rig.feed(begin(4)).empty());
    CHECK(rig.state() == FrameAssembler::State::Open);
    CHECK(completeOnly(rig.feed(end(4, 2))) != nullptr);
}

TEST_CASE("state row: invalid BEGIN with an int snap of 1 or more reports an unsupported format") {
    const std::string oldLayout = "CURIA1|3|BEGIN|1066.9.15|0.1.0|Test Ruler One|more";
    {
        Rig rig;  // Idle
        const auto events = rig.feed(oldLayout);
        const auto* u = onlyEvent<UnsupportedFormat>(events);
        REQUIRE(u != nullptr);
        CHECK(u->snap == 3);
        CHECK(u->leadingFields == "1066.9.15|0.1.0|Test Ruler One");
        CHECK(rig.state() == FrameAssembler::State::Idle);
    }
    {
        Rig rig;  // Open: Damaged (incomplete) first, then the report
        rig.feed({begin(2), r(2, "FIN|1|2")});
        const auto events = rig.feed(oldLayout);
        REQUIRE(events.size() == 2);
        const auto* d = std::get_if<FrameDamaged>(events.data());
        REQUIRE(d != nullptr);
        CHECK(d->reason == DamageReason::Incomplete);
        CHECK(d->snap == 2);
        CHECK(std::get_if<UnsupportedFormat>(&events[1]) != nullptr);
        CHECK(rig.state() == FrameAssembler::State::Idle);
    }
    {
        Rig rig;  // Damaged: only the report
        rig.feed({begin(2), r(2, "FIN|bad|2")});
        const auto events = rig.feed(oldLayout);
        CHECK(onlyEvent<UnsupportedFormat>(events) != nullptr);
        CHECK(rig.state() == FrameAssembler::State::Idle);
    }
}

TEST_CASE(
    "state row: an invalid BEGIN with a snap that does not parse is an ordinary invalid record") {
    Rig rig;
    CHECK(rig.feed(begin(2)).empty());
    CHECK(damagedWith(rig.feed("CURIA1|x|BEGIN|0|0.1.0"), DamageReason::InvalidRecord));
    Rig idle;
    const auto events = idle.feed("CURIA1|x|BEGIN|0|0.1.0");
    const auto* diag = onlyEvent<Diagnostic>(events);
    REQUIRE(diag != nullptr);
    CHECK_FALSE(diag->snap.has_value());
}

TEST_CASE("state row: another known type is ignored in Idle (the watcher attached mid-frame)") {
    Rig rig;
    const auto frame = exampleFrame(7);
    // The first half of the frame is missing; the rest, END included, arrives in Idle.
    const std::vector<std::string> tail(frame.begin() + 8, frame.end());
    CHECK(rig.feed(tail).empty());
    CHECK(rig.state() == FrameAssembler::State::Idle);
    CHECK(completeOnly(rig.feed(exampleFrame(8))) != nullptr);
}

TEST_CASE("state row: another known type in Open is stored; a different snap damages the frame") {
    Rig rig;
    rig.feed({begin(3)});
    const auto events = rig.feed(r(4, "FIN|1|2"));
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::SnapMismatch);
    CHECK(d->snap == 3);
    CHECK(rig.state() == FrameAssembler::State::Damaged);
    // Nothing more is reported for this frame, END included.
    CHECK(rig.feed({r(3, "FIN|1|2"), end(3, 4)}).empty());
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: a shortened text in Open sets the flag, the frame stays Complete") {
    const auto events =
        run({r(5, "VASSALS|1|1"), r(5, "VASSAL|1|2|3|4|5|" + std::string(1500, 'v'))});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->textShortened);
    REQUIRE(frame->vassals.has_value());
    CHECK(must(frame->vassals).listed[0].strength == 5);
}

TEST_CASE("state row: another known type in Damaged is ignored") {
    Rig rig;
    rig.feed({begin(3)});
    rig.feed(r(4, "FIN|1|2"));  // damages
    CHECK(rig.feed({r(3, "FIN|1|2"), r(9, "HEIR|none"), r(3, "FIN|oops")}).empty());
    CHECK(rig.state() == FrameAssembler::State::Damaged);
}

TEST_CASE("state row: an unknown type in Idle is ignored") {
    Rig rig;
    CHECK(rig.feed(r(3, "FOO|1|2")).empty());
    CHECK(rig.feed(r(3, "PROBE|a|b")).empty());
    CHECK(rig.feed(r(3, "FACTION|1|DISCONTENT|4")).empty());
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: an unknown type in Open is counted; a different snap damages the frame") {
    // Counted: END must include it.
    CHECK(completeOnly(run({r(5, "FOO|1"), r(5, "PROBE|x|y"), r(5, "FACTION|1|DISCONTENT|42"),
                            r(5, "WAR|2|SCORE|1")})) != nullptr);
    Rig rig;
    rig.feed({begin(3), r(3, "FOO|1")});
    CHECK(rig.feed(end(3, 2)).size() == 1);  // END count 2 is wrong: BEGIN, FOO and END are 3
    Rig other;
    other.feed({begin(3)});
    CHECK(damagedWith(other.feed(r(4, "FOO|1")), DamageReason::SnapMismatch));
}

TEST_CASE("state row: an unknown type is not stored, and not counted if it is left out of END") {
    // The END count of the frame leaves out the unknown record: count mismatch.
    Rig rig;
    const auto events = rig.feed({begin(3), r(3, "FOO|1"), end(3, 2)});
    CHECK(damagedWith(events, DamageReason::CountMismatch));
}

TEST_CASE("state row: an unknown type in Damaged is ignored") {
    Rig rig;
    rig.feed({begin(3), r(4, "FOO|1")});
    CHECK(rig.feed(r(3, "FOO|1")).empty());
}

TEST_CASE("state row: an invalid record in Idle is only reported to the log") {
    Rig rig;
    const auto events = rig.feed(r(3, "FIN|bad|1"));
    const auto* diag = onlyEvent<Diagnostic>(events);
    REQUIRE(diag != nullptr);
    REQUIRE(diag->snap.has_value());
    CHECK(must(diag->snap) == 3);
    CHECK(diag->reason == InvalidReason::FieldValue);
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: an invalid record in Open damages the frame (invalid record)") {
    Rig rig;
    rig.feed({begin(3), r(3, "FIN|1|2")});
    const auto events = rig.feed(r(3, "HEIR|1|2|1|0"));
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::InvalidRecord);
    CHECK(d->snap == 3);
    CHECK(rig.state() == FrameAssembler::State::Damaged);
    // An invalid record whose snap does not parse damages as well.
    Rig other;
    other.feed(begin(3));
    CHECK(damagedWith(other.feed("CURIA1|??|FIN|1|2"), DamageReason::InvalidRecord));
    // So does one with another snap.
    Rig third;
    third.feed(begin(3));
    CHECK(damagedWith(third.feed(r(9, "FIN|x|2")), DamageReason::InvalidRecord));
}

TEST_CASE("state row: an invalid record in Damaged is ignored") {
    Rig rig;
    rig.feed({begin(3), r(3, "FIN|bad|1")});
    CHECK(rig.feed({r(3, "FIN|bad|1"), "CURIA1|zz|FOO", r(3, "HEIR|1")}).empty());
}

TEST_CASE("state row: records with snap 0 never touch a frame") {
    // Idle: a valid LOAD is ignored, an invalid snap-0 record is reported to the log.
    Rig idle;
    CHECK(idle.feed("CURIA1|0|LOAD|0.1.0").empty());
    const auto events = idle.feed("CURIA1|0|BEGIN|0|0.1.0|1066|9|15|1|x");
    const auto* diag = onlyEvent<Diagnostic>(events);
    REQUIRE(diag != nullptr);
    CHECK(diag->reason == InvalidReason::SnapRule);
    CHECK(idle.state() == FrameAssembler::State::Idle);

    // Open: ignored and not counted, valid or not.
    Rig open;
    open.feed({begin(3), "CURIA1|0|LOAD|0.1.0", r(3, "FIN|1|2")});
    const auto invalidZero = open.feed("CURIA1|0|END|9");
    CHECK(onlyEvent<Diagnostic>(invalidZero) != nullptr);
    CHECK(open.state() == FrameAssembler::State::Open);
    const auto done = open.feed(end(3, 3));  // BEGIN, FIN, END
    CHECK(completeOnly(done) != nullptr);

    // Damaged: ignored.
    Rig damaged;
    damaged.feed({begin(3), r(3, "FIN|bad|1")});
    CHECK(damaged.feed("CURIA1|0|LOAD|0.1.0").empty());
    CHECK(damaged.state() == FrameAssembler::State::Damaged);
}

TEST_CASE("state row: END in Idle is ignored") {
    Rig rig;
    CHECK(rig.feed(end(3, 2)).empty());
    CHECK(rig.feed({end(3, 2), end(3, 2)}).empty());
}

TEST_CASE("state row: END in Open closes a complete frame") {
    Rig rig;
    const auto events = rig.feed(frameOf(3, {r(3, "FIN|1|2")}));
    CHECK(completeOnly(events) != nullptr);
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: END in Open with another snap is a snap mismatch, then Idle") {
    Rig rig;
    rig.feed({begin(3)});
    const auto events = rig.feed(end(4, 2));
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::SnapMismatch);
    CHECK(d->snap == 3);
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: END in Open with a wrong count is a count mismatch, then Idle") {
    for (const std::size_t claimed : {1U, 2U, 4U, 100U}) {
        CAPTURE(claimed);
        Rig rig;
        const auto events = rig.feed({begin(3), r(3, "FIN|1|2"), end(3, claimed)});
        CHECK(damagedWith(events, DamageReason::CountMismatch));
        CHECK(rig.state() == FrameAssembler::State::Idle);
    }
}

TEST_CASE("state row: END precedence is snap mismatch, then count mismatch, then order") {
    // Wrong snap and wrong count: snap mismatch.
    Rig a;
    a.feed({begin(3)});
    CHECK(damagedWith(a.feed(end(4, 99)), DamageReason::SnapMismatch));
    // Wrong count and a wrong order: count mismatch.
    Rig b;
    CHECK(damagedWith(b.feed({begin(3), r(3, "HEIR|none"), r(3, "FIN|1|2"), end(3, 99)}),
                      DamageReason::CountMismatch));
    // Wrong order only: order.
    Rig c;
    CHECK(damagedWith(c.feed({begin(3), r(3, "HEIR|none"), r(3, "FIN|1|2"), end(3, 4)}),
                      DamageReason::Order));
    // A wrong group count and a wrong order elsewhere: count mismatch.
    Rig d;
    CHECK(damagedWith(d.feed(frameOf(3, {r(3, "HEIR|none"), r(3, "FIN|1|2"), r(3, "WARS|1")})),
                      DamageReason::CountMismatch));
}

TEST_CASE("state row: END in Damaged goes to Idle without an event") {
    Rig rig;
    rig.feed({begin(3), r(3, "FIN|bad|1")});
    CHECK(rig.feed(end(3, 3)).empty());
    CHECK(rig.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state row: a double END: the second one is ignored in Idle") {
    Rig rig;
    const auto frame = frameOf(3, {r(3, "FIN|1|2")});
    auto events = rig.feed(frame);
    REQUIRE(completeOnly(events) != nullptr);
    CHECK(rig.feed(frame.back()).empty());
}

TEST_CASE("state row: the 513th frame record makes the frame too large") {
    // BEGIN + 510 unknown + END = 512 records: fine.
    {
        const std::vector<std::string> middle(510, r(5, "FOO|1"));
        CHECK(completeOnly(run(middle)) != nullptr);
    }
    // 511 unknown: END is the 513th record and still gives too large.
    {
        const std::vector<std::string> middle(511, r(5, "FOO|1"));
        const auto events = run(middle);
        const auto* d = damagedOnly(events);
        REQUIRE(d != nullptr);
        CHECK(d->reason == DamageReason::TooLarge);
        CHECK(d->snap == 5);
    }
    // 600 records: one event only, at the 513th; the rest is ignored.
    {
        Rig rig;
        std::vector<std::string> lines{begin(5)};
        for (int i = 0; i < 600; ++i) {
            lines.push_back(r(5, "FOO|1"));
        }
        const auto events = rig.feed(lines);
        REQUIRE(events.size() == 1);
        CHECK(damagedWith(events, DamageReason::TooLarge));
        CHECK(rig.state() == FrameAssembler::State::Damaged);
    }
}

TEST_CASE("state row: known records count toward the 512 limit too") {
    std::vector<std::string> middle;
    middle.push_back(r(5, "VASSALS|600|600"));
    for (int i = 1; i <= 600; ++i) {
        middle.push_back(r(5, "VASSAL|" + std::to_string(i) + "|1|2|3|4|x"));
    }
    CHECK(damagedWith(run(middle), DamageReason::TooLarge));
}

TEST_CASE("state row: frame-record bytes above 128 KiB make the frame too large") {
    // The numbers of the spec (section 3), written out so that changing the constant fails a test.
    constexpr std::size_t kCap = 128 * 1024;
    static_assert(FrameAssembler::kMaxFrameBytes == 128 * 1024);
    static_assert(FrameAssembler::kMaxFrameRecords == 512);
    const std::string b = begin(5);
    const auto buildFrame = [&](std::size_t extraByte) {
        // BEGIN, 43 unknown records of 3000 bytes, one unknown record sized to land the sum
        // exactly on the cap (plus extraByte), and END.
        constexpr std::size_t kFill = 3000;
        constexpr std::size_t kFillCount = 43;
        const std::size_t records = 1 + kFillCount + 1 + 1;
        const std::string endRecord = end(5, records);
        const std::size_t rest = b.size() + kFillCount * kFill + endRecord.size();
        std::vector<std::string> lines{b};
        for (std::size_t i = 0; i < kFillCount; ++i) {
            lines.push_back("CURIA1|5|FOO|" + std::string(kFill - 13, 'x'));
        }
        const std::size_t remainder = kCap + extraByte - rest;
        lines.push_back("CURIA1|5|FOO|" + std::string(remainder - 13, 'y'));
        lines.push_back(endRecord);
        return lines;
    };
    {
        Rig rig;
        CHECK(completeOnly(rig.feed(buildFrame(0))) != nullptr);  // exactly 128 KiB is allowed
    }
    {
        Rig rig;
        const auto events = rig.feed(buildFrame(1));
        CHECK(damagedWith(events, DamageReason::TooLarge));
    }
}

TEST_CASE("state row: the BEGIN that abandons a frame never counts toward the old frame's limits") {
    Rig rig;
    std::vector<std::string> lines{begin(5)};
    for (int i = 0; i < 511; ++i) {
        lines.push_back(r(5, "FOO|1"));
    }
    // 512 records so far (BEGIN + 511); a new BEGIN would be the 513th for the old frame if it
    // counted, and it must not be too large: the old frame is incomplete, the new one starts at 1.
    const auto first = rig.feed(lines);
    CHECK(first.empty());
    const auto events = rig.feed(begin(6));
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::Incomplete);
    // The new frame starts with a count of 1: it can take 510 more records and an END.
    std::vector<std::string> more(510, r(6, "FOO|1"));
    more.push_back(end(6, 512));
    const auto done = rig.feed(more);
    CHECK(completeOnly(done) != nullptr);
}

TEST_CASE("state row: truncation in Idle, Open and Damaged") {
    Rig idle;
    CHECK(idle.truncate().empty());
    CHECK(idle.state() == FrameAssembler::State::Idle);

    Rig open;
    open.feed({begin(3), r(3, "FIN|1|2")});
    const auto events = open.truncate();
    const auto* d = damagedOnly(events);
    REQUIRE(d != nullptr);
    CHECK(d->reason == DamageReason::TruncatedLog);
    CHECK(d->snap == 3);
    CHECK(open.state() == FrameAssembler::State::Idle);
    // The rest of the old frame, arriving in the new log, is ignored.
    CHECK(open.feed(end(3, 3)).empty());

    Rig damaged;
    damaged.feed({begin(3), r(3, "FIN|bad|1")});
    CHECK(damaged.truncate().empty());
    CHECK(damaged.state() == FrameAssembler::State::Idle);
}

TEST_CASE("state: exactly one Damaged event per frame, whatever follows the damage") {
    Rig rig;
    auto events = rig.feed({begin(3), r(3, "FIN|bad|1"), r(3, "FIN|bad|1"), r(4, "FIN|1|2"),
                            r(3, "BEGINX|1"), "CURIA1|3|", end(3, 9)});
    CHECK(eventsOfType<FrameDamaged>(events).size() == 1);
}

// ---- arrival order, repeated and falling snapshots ----------------------------------------------

TEST_CASE("assembler: snapshot numbers can fall or repeat; every complete frame is reported") {
    Rig rig;
    auto events = rig.feed(exampleFrame(7));
    auto more = rig.feed(exampleFrame(3));
    events.insert(events.end(), more.begin(), more.end());
    more = rig.feed(exampleFrame(3));
    events.insert(events.end(), more.begin(), more.end());
    const auto frames = eventsOfType<FrameComplete>(events);
    REQUIRE(frames.size() == 3);
    CHECK(frames[0]->frame.snap == 7);
    CHECK(frames[1]->frame.snap == 3);
    CHECK(frames[2]->frame.snap == 3);
    CHECK(eventsOfType<FrameDamaged>(events).empty());
}

// ---- checked counts and section rules
// ------------------------------------------------------------

TEST_CASE("count row VASSALS: listed equals the VASSAL records") {
    CHECK(damagedWith(run({r(5, "VASSALS|2|2"), r(5, "VASSAL|1|1|1|1|1|a")}),
                      DamageReason::CountMismatch));
    CHECK(damagedWith(
        run({r(5, "VASSALS|2|1"), r(5, "VASSAL|1|1|1|1|1|a"), r(5, "VASSAL|2|1|1|1|1|b")}),
        DamageReason::CountMismatch));
    CHECK(completeOnly(run({r(5, "VASSALS|9|2"), r(5, "VASSAL|1|1|1|1|1|a"),
                            r(5, "VASSAL|2|1|1|1|1|b")})) != nullptr);
}

TEST_CASE("count row VASSALS: listed is at most total") {
    CHECK(damagedWith(
        run({r(5, "VASSALS|1|2"), r(5, "VASSAL|1|1|1|1|1|a"), r(5, "VASSAL|2|1|1|1|1|b")}),
        DamageReason::CountMismatch));
}

TEST_CASE("count row VASSALS: rank runs 1..listed in order") {
    CHECK(damagedWith(
        run({r(5, "VASSALS|2|2"), r(5, "VASSAL|2|1|1|1|1|a"), r(5, "VASSAL|1|1|1|1|1|b")}),
        DamageReason::Order));
    CHECK(damagedWith(
        run({r(5, "VASSALS|2|2"), r(5, "VASSAL|1|1|1|1|1|a"), r(5, "VASSAL|1|1|1|1|1|b")}),
        DamageReason::Order));
}

namespace {

std::string warGroup(std::int32_t snap, int n) {
    const std::string num = std::to_string(n);
    return r(snap, "WAR|" + num + "|NAME|Test War " + num) + "\n" +
           r(snap, "WAR|" + num + "|ATTACKER|2000|Test Attacker " + num) + "\n" +
           r(snap, "WAR|" + num + "|DEFENDER|1001|Test Ruler One");
}

std::vector<std::string> split(const std::string& lines) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    while (pos <= lines.size()) {
        const std::size_t nl = lines.find('\n', pos);
        out.push_back(lines.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos));
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    return out;
}

std::vector<std::string> warsMiddle(const std::string& header, std::initializer_list<int> groups) {
    std::vector<std::string> middle{header};
    for (const int n : groups) {
        for (std::string& line : split(warGroup(5, n))) {
            middle.push_back(std::move(line));
        }
    }
    return middle;
}

}  // namespace

TEST_CASE("count row WARS: total equals the WAR groups, n runs 1..total") {
    {
        const auto events = run(warsMiddle(r(5, "WARS|2"), {1, 2}));
        const Frame* frame = completeOnly(events);
        REQUIRE(frame != nullptr);
        REQUIRE(frame->wars.has_value());
        REQUIRE(must(frame->wars).wars.size() == 2);
        CHECK(must(frame->wars).wars[1].name == "Test War 2");
        CHECK(must(frame->wars).wars[1].attacker.id == 2000);
        CHECK(must(frame->wars).wars[1].defender.name == "Test Ruler One");
    }
    CHECK(damagedWith(run(warsMiddle(r(5, "WARS|3"), {1, 2})), DamageReason::CountMismatch));
    CHECK(damagedWith(run(warsMiddle(r(5, "WARS|1"), {1, 2})), DamageReason::CountMismatch));
    CHECK(damagedWith(run(warsMiddle(r(5, "WARS|2"), {1, 3})), DamageReason::Order));
    CHECK(damagedWith(run(warsMiddle(r(5, "WARS|2"), {2, 1})), DamageReason::Order));
}

TEST_CASE("count row WARS: each group is NAME, ATTACKER, DEFENDER adjacent, once each") {
    // DEFENDER before ATTACKER.
    CHECK(damagedWith(run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|1|DEFENDER|1|d"),
                           r(5, "WAR|1|ATTACKER|2|a")}),
                      DamageReason::Order));
    // A missing DEFENDER.
    CHECK(damagedWith(run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|1|ATTACKER|2|a")}),
                      DamageReason::Order));
    // A repeated ATTACKER.
    CHECK(damagedWith(run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|1|ATTACKER|2|a"),
                           r(5, "WAR|1|ATTACKER|2|a"), r(5, "WAR|1|DEFENDER|1|d")}),
                      DamageReason::Order));
    // A group whose records carry different n.
    CHECK(damagedWith(run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|2|ATTACKER|2|a"),
                           r(5, "WAR|1|DEFENDER|1|d")}),
                      DamageReason::Order));
    // A WAR record without a WARS header.
    CHECK(damagedWith(run({r(5, "WAR|1|NAME|w")}), DamageReason::Order));
}

TEST_CASE("count row WARS: unknown sub-kinds and types inside a group are skipped") {
    const auto events =
        run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|1|SCORE|55"), r(5, "FOO|1"),
             r(5, "WAR|1|ATTACKER|2|a"), r(5, "PROBE|x"), r(5, "WAR|1|DEFENDER|1|d")});
    CHECK(completeOnly(events) != nullptr);
}

TEST_CASE("count row WARS: the ruler as an ally is told from the ids") {
    const auto events = run({r(5, "WARS|1"), r(5, "WAR|1|NAME|w"), r(5, "WAR|1|ATTACKER|2|a"),
                             r(5, "WAR|1|DEFENDER|3|d")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->wars.has_value());
    REQUIRE(must(frame->wars).wars.size() == 1);
    CHECK(must(frame->wars).wars[0].attacker.id != frame->rulerId);
    CHECK(must(frame->wars).wars[0].defender.id != frame->rulerId);
}

TEST_CASE(
    "count row CLAIMS: listed equals the CLAIM records, n runs 1..listed, listed <= explicit") {
    CHECK(completeOnly(run({r(5, "CLAIMS|2|1|2"), r(5, "CLAIM|1|strong|5|a"),
                            r(5, "CLAIM|2|weak|1|b")})) != nullptr);
    CHECK(damagedWith(run({r(5, "CLAIMS|2|1|2"), r(5, "CLAIM|1|strong|5|a")}),
                      DamageReason::CountMismatch));
    CHECK(damagedWith(
        run({r(5, "CLAIMS|2|1|1"), r(5, "CLAIM|1|strong|5|a"), r(5, "CLAIM|2|weak|1|b")}),
        DamageReason::CountMismatch));
    CHECK(damagedWith(
        run({r(5, "CLAIMS|1|0|2"), r(5, "CLAIM|1|strong|5|a"), r(5, "CLAIM|2|weak|1|b")}),
        DamageReason::CountMismatch));
    CHECK(damagedWith(
        run({r(5, "CLAIMS|2|0|2"), r(5, "CLAIM|1|strong|5|a"), r(5, "CLAIM|3|weak|1|b")}),
        DamageReason::Order));
    CHECK(damagedWith(run({r(5, "CLAIM|1|strong|5|a")}), DamageReason::Order));
}

namespace {

std::vector<std::string> factionGroup(int n, const std::string& role,
                                      const std::vector<std::string>& skipSubKinds = {}) {
    const std::string num = std::to_string(n);
    std::vector<std::string> all = {
        r(5, "FACTION|" + num + "|INFO|" + role + "|3|33|100|2"),
        r(5, "FACTION|" + num + "|LEADER|1004|Test Vassal Four"),
        r(5, "FACTION|" + num + "|CLAIMANT|1005|Test Claimant Five"),
        r(5, "FACTION|" + num + "|TITLE|4|Test Realm"),
        r(5, "FACTION|" + num + "|TARGET|1001|Test Ruler One"),
    };
    std::vector<std::string> out;
    for (const std::string& line : all) {
        bool skip = false;
        for (const std::string& s : skipSubKinds) {
            skip = skip || line.find("|" + s + "|") != std::string::npos;
        }
        if (!skip) {
            out.push_back(line);
        }
    }
    return out;
}

std::vector<std::string> concat(std::vector<std::string> a, const std::vector<std::string>& b) {
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace

TEST_CASE("count row FACTIONS: listed equals the groups, n runs 1..listed") {
    CHECK(completeOnly(run(concat({r(5, "FACTIONS|2|0|2")}, concat(factionGroup(1, "against"),
                                                                   factionGroup(2, "against"))))) !=
          nullptr);
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|2|0|2")}, factionGroup(1, "against"))),
                      DamageReason::CountMismatch));
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|2|0|1")},
                                 concat(factionGroup(1, "against"), factionGroup(2, "against")))),
                      DamageReason::CountMismatch));
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|2|0|2")},
                                 concat(factionGroup(1, "against"), factionGroup(3, "against")))),
                      DamageReason::Order));
}

TEST_CASE("count row FACTIONS: each group is INFO, LEADER, CLAIMANT, TITLE, TARGET once each") {
    for (const char* missing : {"LEADER", "CLAIMANT", "TITLE", "TARGET"}) {
        CAPTURE(missing);
        CHECK(damagedWith(
            run(concat({r(5, "FACTIONS|1|0|1")}, factionGroup(1, "against", {missing}))),
            DamageReason::Order));
    }
    // INFO missing: the group starts with LEADER.
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|0|1")}, factionGroup(1, "against", {"INFO"}))),
                      DamageReason::CountMismatch));
    // Swapped LEADER and CLAIMANT.
    auto swapped = factionGroup(1, "against");
    std::swap(swapped[1], swapped[2]);
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|0|1")}, swapped)), DamageReason::Order));
    // One record of the group with another n.
    auto wrongN = factionGroup(1, "against");
    wrongN[3] = r(5, "FACTION|2|TITLE|4|Test Realm");
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|0|1")}, wrongN)), DamageReason::Order));
}

TEST_CASE("count row FACTIONS: groups with role member equal member; against <= against") {
    CHECK(completeOnly(run(concat({r(5, "FACTIONS|1|1|2")}, concat(factionGroup(1, "against"),
                                                                   factionGroup(2, "member"))))) !=
          nullptr);
    // member says 0, but a member group is listed.
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|0|2")},
                                 concat(factionGroup(1, "against"), factionGroup(2, "member")))),
                      DamageReason::CountMismatch));
    // member says 1, none listed.
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|1|1")}, factionGroup(1, "against"))),
                      DamageReason::CountMismatch));
    // two member groups.
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|0|1|2")},
                                 concat(factionGroup(1, "member"), factionGroup(2, "member")))),
                      DamageReason::CountMismatch));
    // against listed more than against says.
    CHECK(damagedWith(run(concat({r(5, "FACTIONS|1|0|2")},
                                 concat(factionGroup(1, "against"), factionGroup(2, "against")))),
                      DamageReason::CountMismatch));
    // against may be larger than listed (the mod caps the list).
    CHECK(completeOnly(run(concat({r(5, "FACTIONS|9|0|1")}, factionGroup(1, "against")))) !=
          nullptr);
}

TEST_CASE("count row FACTIONS: the none forms become std::nullopt") {
    const auto events = run({r(5, "FACTIONS|1|1|1"), r(5, "FACTION|1|INFO|member|1|10|100|1"),
                             r(5, "FACTION|1|LEADER|none"), r(5, "FACTION|1|CLAIMANT|none"),
                             r(5, "FACTION|1|TITLE|none"), r(5, "FACTION|1|TARGET|none")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->factions.has_value());
    REQUIRE(must(frame->factions).listed.size() == 1);
    const Faction& f = must(frame->factions).listed[0];
    CHECK(f.role == FactionRole::Member);
    CHECK_FALSE(f.leader.has_value());
    CHECK_FALSE(f.claimant.has_value());
    CHECK_FALSE(f.title.has_value());
    CHECK_FALSE(f.target.has_value());
}

TEST_CASE("count row FACTIONS: unknown sub-kinds inside a group are skipped") {
    auto group = factionGroup(1, "against");
    group.insert(group.begin() + 2, r(5, "FACTION|1|DISCONTENT|42"));
    group.insert(group.begin() + 4, r(5, "FACTION|1|AT_WAR|0"));
    CHECK(completeOnly(run(concat({r(5, "FACTIONS|1|0|1")}, group))) != nullptr);
}

TEST_CASE("section rules: order and repetition") {
    // Out of order: HEIR before FIN.
    CHECK(damagedWith(run({r(5, "HEIR|none"), r(5, "FIN|1|2")}), DamageReason::Order));
    // Sections in order with gaps are fine.
    CHECK(completeOnly(run({r(5, "FIN|1|2"), r(5, "WARS|0"), r(5, "FACTIONS|0|0|0")})) != nullptr);
    // Repetition.
    CHECK(damagedWith(run({r(5, "FIN|1|2"), r(5, "FIN|1|2")}), DamageReason::Order));
    CHECK(damagedWith(run({r(5, "VASSALS|0|0"), r(5, "VASSALS|0|0")}), DamageReason::Order));
    CHECK(damagedWith(run({r(5, "HEIR|none"), r(5, "HEIR|none")}), DamageReason::Order));
    // An item after the next section's header.
    CHECK(damagedWith(run({r(5, "VASSALS|1|1"), r(5, "WARS|0"), r(5, "VASSAL|1|1|1|1|1|a")}),
                      DamageReason::CountMismatch));
    // An item of a section whose header was skipped.
    CHECK(damagedWith(run({r(5, "FIN|1|2"), r(5, "CLAIM|1|weak|1|a")}), DamageReason::Order));
}

TEST_CASE("section rules: unknown records are ignored for order and adjacency") {
    const auto events =
        run({r(5, "FIN|1|2"), r(5, "PROBE|x|y"), r(5, "VASSALS|2|2"), r(5, "NEWTYPE|1"),
             r(5, "VASSAL|1|1|1|1|1|a"), r(5, "FOO"), r(5, "VASSAL|2|1|1|1|1|b"), r(5, "BAR|1|2")});
    CHECK(completeOnly(events) != nullptr);
}

TEST_CASE("section rules: a PROBE frame is a frame like any other") {
    const auto events = run({r(5, "PROBE|claim detail|x|y"), r(5, "FIN|1|2")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->recordCount == 4);
}

TEST_CASE("assembler: a frame with mode 1 is reported like any other") {
    Rig rig;
    const auto events = rig.feed({r(5, "BEGIN|1|0.1.0|1066|9|15|1001|x"), end(5, 2)});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->mode == 1);
}

TEST_CASE("assembler: a text-shortened vassal does not hide the fixed fields") {
    const auto events =
        run({r(5, "VASSALS|1|1"), r(5, "VASSAL|1|2|-3|-4|5|" + std::string(2000, 'x'))});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->vassals.has_value());
    CHECK(must(frame->vassals).listed[0].gold == -3);
    CHECK(must(frame->vassals).listed[0].opinion == -4);
    CHECK(frame->textShortened);
}

TEST_CASE("assembler: an oversize record without text damages the frame as invalid") {
    CHECK(
        damagedWith(run({r(5, "FIN|1|2" + std::string(2000, ' '))}), DamageReason::InvalidRecord));
}

// ---- the whole chain: bytes -> lines -> records -> events
// ------------------------------------------

namespace {

std::string streamOf(const std::vector<std::string>& bare, const char* lineEnd = "\n") {
    std::string out;
    std::size_t i = 0;
    for (const std::string& record : bare) {
        out += curia::bridge::test::prefixed(record, i++);
        out += lineEnd;
    }
    return out;
}

}  // namespace

TEST_CASE("pipeline: CRLF line ends are fine, a CR inside a record is not") {
    {
        BridgePipeline pipeline;
        const auto events = pipeline.feed(streamOf(exampleFrame(7), "\r\n"));
        const Frame* frame = completeOnly(events);
        REQUIRE(frame != nullptr);
        CHECK(frame->rulerName == "Test Ruler One");  // no CR left in the last field of a line
    }
    {
        BridgePipeline pipeline;
        auto frame = frameOf(7, {r(7, "FIN|312|9\r|x")});  // a CR that is not before the LF
        CHECK(damagedWith(pipeline.feed(streamOf(frame, "\n")), DamageReason::InvalidRecord));
    }
    {
        BridgePipeline pipeline;
        auto frame = frameOf(7, {r(7, "FIN|312|9\r\r")});  // two CR: one is removed, one stays
        CHECK(damagedWith(pipeline.feed(streamOf(frame, "\n")), DamageReason::InvalidRecord));
    }
}

TEST_CASE("pipeline: an unterminated last line is held back until its newline arrives") {
    BridgePipeline pipeline;
    const auto frame = exampleFrame(7);
    std::string bytes = streamOf(frame);
    bytes.pop_back();  // the final newline
    CHECK(pipeline.feed(bytes).empty());
    CHECK(completeOnly(pipeline.feed("\n")) != nullptr);
}

TEST_CASE("pipeline: truncating the log drops the unterminated tail") {
    BridgePipeline pipeline;
    // An unterminated BEGIN: if it survived the truncation, it would be glued to the first line of
    // the new log and open a frame of its own.
    const std::string tail = curia::bridge::test::prefixed(begin(7));
    CHECK(pipeline.feed(tail).empty());
    CHECK(pipeline.splitter().pendingBytes() == tail.size());
    CHECK(pipeline.truncateLog().empty());  // Idle: no frame was open
    CHECK(pipeline.splitter().pendingBytes() == 0);
    const auto events = pipeline.feed(streamOf(frameOf(9, {})));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->snap == 9);
}

TEST_CASE("pipeline: truncating the log also forgets that a line was overlong") {
    BridgePipeline pipeline;
    CHECK(pipeline.feed(std::string(6000, 'x')).empty());  // an overlong line, no newline yet
    CHECK(pipeline.truncateLog().empty());
    // The first line of the new log is an ordinary short line, not "the rest of an overlong one".
    const auto events = pipeline.feed(streamOf(frameOf(9, {r(9, "HEIR|1|2|1|0|Test Heir")})));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK_FALSE(frame->textShortened);
}

TEST_CASE(
    "pipeline: an overlong line gives a shortened record and does not swallow the next line") {
    BridgePipeline pipeline;
    std::string bytes;
    bytes += curia::bridge::test::prefixedLine(begin(5));
    bytes += curia::bridge::test::prefixedLine(r(5, "VASSALS|1|1"));
    // 5,000 bytes of name: the head of the line is kept, the tail (which even contains a record) is
    // dropped.
    bytes += curia::bridge::test::prefixed(r(5, "VASSAL|1|2|3|4|5|") + std::string(5000, 'v') +
                                           " CURIA1|5|END|3");
    bytes += "\n";
    bytes += curia::bridge::test::prefixedLine(end(5, 4));
    const auto events = pipeline.feed(bytes);
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->textShortened);
    REQUIRE(frame->vassals.has_value());
    REQUIRE(must(frame->vassals).listed.size() == 1);
    CHECK(must(frame->vassals).listed[0].strength == 5);
    CHECK(must(frame->vassals).listed[0].name.size() < 1024);
    CHECK(must(frame->vassals).listed[0].name.find("CURIA1") == std::string::npos);
}

TEST_CASE("pipeline: an overlong line without a text field is an invalid record") {
    BridgePipeline pipeline;
    std::string bytes;
    bytes += curia::bridge::test::prefixedLine(begin(5));
    bytes += curia::bridge::test::prefixed(r(5, "FIN|1|2")) + std::string(5000, ' ') + "\n";
    const auto events = pipeline.feed(bytes);
    CHECK(damagedWith(events, DamageReason::InvalidRecord));
}

TEST_CASE("pipeline: invalid UTF-8 in a name is repaired, the frame is kept") {
    BridgePipeline pipeline;
    const std::string bad = std::string("Test ") + "\xFF\xFE" + " Name \xE2\x82";
    const auto events = pipeline.feed(streamOf(frameOf(5, {r(5, "HEIR|1|2|1|0|" + bad)})));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->heir.has_value());
    CHECK(must(frame->heir).name == "Test \xEF\xBF\xBD\xEF\xBF\xBD Name \xEF\xBF\xBD");
}

TEST_CASE("pipeline: a NUL byte inside a text field is just removed by the hygiene") {
    BridgePipeline pipeline;
    const std::string withNul = std::string("Te") + std::string(1, '\0') + "st";
    const auto events = pipeline.feed(streamOf(frameOf(5, {r(5, "HEIR|1|2|1|0|" + withNul)})));
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(must(frame->heir).name == "Test");
}

// ---- wiring of the overlong flag from the splitter to the parser -------------------------------

namespace {

// One physical line without an engine prefix: `lead` (no marker), the record, and `rest`. The lead
// is sized so that the record ends `recordBytesInHead` bytes after the first 4,096 bytes begin to
// matter: with rest non-empty the line is overlong.
std::string overlongLine(const std::string& record, std::size_t headBytesOfRecord,
                         const std::string& rest) {
    return std::string(4096 - headBytesOfRecord, '.') + record + rest;
}

}  // namespace

TEST_CASE("pipeline: a text record from an overlong line is shortened even if its head is short") {
    const std::string heir = r(5, "HEIR|1|2|1|0|Test Heir Two ");
    std::string bytes = prefixedLine(begin(5));
    // ~100 bytes of the record are in the head, the rest of the line is dropped.
    bytes += overlongLine(heir, 100, std::string(1000, 'z')) + "\n";
    bytes += prefixedLine(end(5, 3));
    BridgePipeline pipeline;
    const auto events = pipeline.feed(bytes);
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->textShortened);
    REQUIRE(frame->heir.has_value());
    CHECK(must(frame->heir).name.rfind("Test Heir Two zzz", 0) == 0);
    CHECK(must(frame->heir).name.size() < 100);
}

TEST_CASE("pipeline: a record without text from an overlong line is invalid, even if it is whole") {
    const std::string fin = r(5, "FIN|1|2");
    std::string bytes = prefixedLine(begin(5));
    // The head ends exactly after the last field: only the overlong flag makes this invalid.
    bytes += overlongLine(fin, fin.size(), "zzzz") + "\n";
    bytes += prefixedLine(end(5, 3));
    BridgePipeline pipeline;
    const auto events = pipeline.feed(bytes);
    const auto* damaged = damagedOnly(events);
    REQUIRE(damaged != nullptr);
    CHECK((damaged->reason == DamageReason::InvalidRecord));
    // The same line without the dropped tail is a valid FIN.
    BridgePipeline control;
    std::string ok = prefixedLine(begin(5));
    ok += overlongLine(fin, fin.size(), "") + "\n";
    ok += prefixedLine(end(5, 3));
    CHECK(completeOnly(control.feed(ok)) != nullptr);
}

TEST_CASE("pipeline: the UTF-8 tail trim also applies to a short head of an overlong line") {
    // The head is cut inside the first 3-byte character of the name, long before 1,024 bytes of
    // record: the half character is dropped, not repaired into a replacement character.
    const std::string heir = r(5, "HEIR|1|2|1|0|Test ");
    std::string bytes = prefixedLine(begin(5));
    bytes += std::string(4096 - heir.size() - 2, '.') + heir + "\xE2\x82\xAC\xE2\x82\xAC" + "\n";
    bytes += prefixedLine(end(5, 3));
    BridgePipeline pipeline;
    const auto events = pipeline.feed(bytes);
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    REQUIRE(frame->heir.has_value());
    CHECK(must(frame->heir).name == "Test");
    CHECK(frame->textShortened);
}

// ---- snap mismatch in both directions ----------------------------------------------------------

TEST_CASE("state row: a LOWER snap damages the frame too, for every kind of record") {
    {
        Rig rig;
        rig.feed({begin(9)});
        CHECK(damagedWith(rig.feed(r(3, "FIN|1|2")), DamageReason::SnapMismatch));
    }
    {
        Rig rig;
        rig.feed({begin(9)});
        CHECK(damagedWith(rig.feed(r(3, "FOO|1")), DamageReason::SnapMismatch));
    }
    {
        Rig rig;
        rig.feed({begin(9)});
        const auto events = rig.feed(r(3, "END|2"));
        CHECK(damagedWith(events, DamageReason::SnapMismatch));
        CHECK(rig.state() == FrameAssembler::State::Idle);
    }
    {
        // The foreign record is never merged: no Complete frame comes out of this.
        Rig rig;
        const auto events = rig.feed({begin(9), r(3, "FIN|1|2"), end(9, 3)});
        CHECK(damagedWith(events, DamageReason::SnapMismatch));
        CHECK(eventsOfType<FrameComplete>(events).empty());
    }
}

// ---- hygiene reaches every text field -----------------------------------------------------------

TEST_CASE("assembler: hygiene is applied to every text-bearing record kind") {
    const std::string u(1, '\x15');
    const std::string messy = "Install " + u + "ONCLICK:CHARACTER,1 " + u + "TOOLTIP:CHARACTER,1 " +
                              u + "L Test Ruler" + u + "!" + u + "!" + u + "! on the " + u +
                              "ONCLICK:TITLE,2 " + u + "TOOLTIP:LANDED_TITLE,2 " + u +
                              "L; Test Realm" + u + "!" + u + "!" + u + "! Throne";
    const std::string clean = "Install Test Ruler on the Test Realm Throne";
    const std::vector<std::string> lines = {
        r(5, "BEGIN|0|0.1.0|1066|9|15|1001|" + messy),
        r(5, "HEIR|1|2|1|0|" + messy),
        r(5, "VASSALS|1|1"),
        r(5, "VASSAL|1|2|3|4|5|" + messy),
        r(5, "WARS|1"),
        r(5, "WAR|1|NAME|" + messy),
        r(5, "WAR|1|ATTACKER|2|" + messy),
        r(5, "WAR|1|DEFENDER|3|" + messy),
        r(5, "CLAIMS|1|0|1"),
        r(5, "CLAIM|1|weak|4|" + messy),
        r(5, "FACTIONS|1|0|1"),
        r(5, "FACTION|1|INFO|against|3|1|1|1"),
        r(5, "FACTION|1|LEADER|4|" + messy),
        r(5, "FACTION|1|CLAIMANT|5|" + messy),
        r(5, "FACTION|1|TITLE|4|" + messy),
        r(5, "FACTION|1|TARGET|6|" + messy),
        r(5, "END|17"),
    };
    Rig rig;
    const auto events = rig.feed(lines);
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    CHECK(frame->rulerName == clean);
    CHECK(must(frame->heir).name == clean);
    CHECK(must(frame->vassals).listed.at(0).name == clean);
    CHECK(must(frame->wars).wars.at(0).name == clean);
    CHECK(must(frame->wars).wars.at(0).attacker.name == clean);
    CHECK(must(frame->wars).wars.at(0).defender.name == clean);
    CHECK(must(frame->claims).listed.at(0).title == clean);
    const Faction& faction = must(frame->factions).listed.at(0);
    CHECK(must(faction.leader).name == clean);
    CHECK(must(faction.claimant).name == clean);
    CHECK(must(faction.title).title == clean);
    CHECK(must(faction.target).name == clean);
}

// ---- CLAIMANT and TITLE outside type 3 ---------------------------------------------------------

TEST_CASE("assembler: a claimant and a title in a faction that is not type 3 are not kept") {
    for (const int type : {1, 2, 4, 5, 6, 7}) {
        CAPTURE(type);
        const std::string t = std::to_string(type);
        const auto events =
            run({r(5, "FACTIONS|1|0|1"), r(5, "FACTION|1|INFO|against|" + t + "|1|1|1"),
                 r(5, "FACTION|1|LEADER|4|Test Leader"), r(5, "FACTION|1|CLAIMANT|5|Test Special"),
                 r(5, "FACTION|1|TITLE|4|Test Title"), r(5, "FACTION|1|TARGET|6|Test Target")});
        const Frame* frame = completeOnly(events);
        REQUIRE(frame != nullptr);  // the values are valid records: the frame stays Complete
        const Faction& faction = must(frame->factions).listed.at(0);
        CHECK_FALSE(faction.claimant.has_value());
        CHECK_FALSE(faction.title.has_value());
        CHECK(must(faction.leader).name == "Test Leader");
        CHECK(must(faction.target).name == "Test Target");
    }
    const auto events =
        run({r(5, "FACTIONS|1|0|1"), r(5, "FACTION|1|INFO|against|3|1|1|1"),
             r(5, "FACTION|1|LEADER|4|Test Leader"), r(5, "FACTION|1|CLAIMANT|5|Test Claimant"),
             r(5, "FACTION|1|TITLE|4|Test Title"), r(5, "FACTION|1|TARGET|6|Test Target")});
    const Frame* frame = completeOnly(events);
    REQUIRE(frame != nullptr);
    const Faction& faction = must(frame->factions).listed.at(0);
    CHECK(must(faction.claimant).name == "Test Claimant");
    CHECK(must(faction.title).tier == 4);
}

// ---- choices the spec does not spell out (pinned so that a change is deliberate) ----------------

TEST_CASE("choice: a count violation wins over an order violation at END") {
    // VASSALS listed 2 but total 1 (count) and the ranks are swapped (order).
    CHECK(damagedWith(
        run({r(5, "VASSALS|1|2"), r(5, "VASSAL|2|1|1|1|1|a"), r(5, "VASSAL|1|1|1|1|1|b")}),
        DamageReason::CountMismatch));
    // The same records with total 2: only the order is wrong.
    CHECK(damagedWith(
        run({r(5, "VASSALS|2|2"), r(5, "VASSAL|2|1|1|1|1|a"), r(5, "VASSAL|1|1|1|1|1|b")}),
        DamageReason::Order));
}

TEST_CASE("choice: too large is decided before snap mismatch") {
    // The 513th frame record has a foreign snap.
    std::vector<std::string> lines{begin(5)};
    for (int i = 0; i < 511; ++i) {
        lines.push_back(r(5, "FOO|1"));
    }
    lines.push_back(r(6, "FOO|1"));
    Rig rig;
    CHECK(damagedWith(rig.feed(lines), DamageReason::TooLarge));
}

TEST_CASE("choice: unknown records count with their full length toward the 128 KiB sum") {
    // 34 unknown records of about 3,900 bytes: 133,000 bytes, above 128 KiB, although a cut at
    // 1,024 bytes (which does not apply to unknown types) would have kept the sum at 35 KiB.
    std::vector<std::string> lines{begin(5)};
    for (int i = 0; i < 34; ++i) {
        lines.push_back(r(5, "FOO|" + std::string(3900, 'x')));
    }
    Rig rig;
    CHECK(damagedWith(rig.feed(lines), DamageReason::TooLarge));
}

// ---- bounded memory and diagnostics
// ---------------------------------------------------------------

TEST_CASE("pipeline: diagnostics are capped between two frame events and counted") {
    BridgePipeline pipeline;
    std::string bytes;
    for (int i = 0; i < 200; ++i) {
        bytes += prefixedLine(r(5, "FIN|bad|1"), static_cast<std::size_t>(i));
    }
    const auto events = pipeline.feed(bytes);
    CHECK(eventsOfType<Diagnostic>(events).size() == BridgePipeline::kMaxDiagnosticsInARun);
    CHECK(pipeline.suppressedDiagnostics() == 200 - BridgePipeline::kMaxDiagnosticsInARun);
    // A frame event ends the run: the next invalid record is reported again.
    CHECK(completeOnly(pipeline.feed(streamOf(frameOf(6, {})))) != nullptr);
    CHECK(eventsOfType<Diagnostic>(pipeline.feed(prefixedLine(r(5, "FIN|bad|1")))).size() == 1);
}

TEST_CASE("pipeline: the diagnostic cap does not depend on how the bytes are chunked") {
    std::string bytes;
    for (int i = 0; i < 150; ++i) {
        bytes += prefixedLine(r(5, "FIN|bad|1"), static_cast<std::size_t>(i));
    }
    bytes += streamOf(frameOf(6, {}));
    for (int i = 0; i < 70; ++i) {
        bytes += prefixedLine(r(5, "FIN|bad|1"), static_cast<std::size_t>(i));
    }
    BridgePipeline whole;
    const auto expected = whole.feed(bytes);
    BridgePipeline chunked;
    std::vector<Event> got;
    for (std::size_t pos = 0; pos < bytes.size(); pos += 7) {
        for (Event& e : chunked.feed(std::string_view(bytes).substr(pos, 7))) {
            got.push_back(std::move(e));
        }
    }
    CHECK(got.size() == expected.size());
    CHECK(chunked.suppressedDiagnostics() == whole.suppressedDiagnostics());
    CHECK(whole.suppressedDiagnostics() == 86 + 6);
}

TEST_CASE("pipeline: a large block of empty lines produces no events and no per-line storage") {
    BridgePipeline pipeline;
    const std::string block(4U * 1024U * 1024U, '\n');
    CHECK(pipeline.feed(block).empty());
    CHECK(pipeline.splitter().pendingBytes() == 0);
}
