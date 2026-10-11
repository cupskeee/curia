// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <ostream>
#include <string>
#include <string_view>
#include <vector>

#include "bridge/record.h"
#include "bridge/text_hygiene.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using curia::bridge::test::must;
using curia::bridge::test::parseBare;
using curia::bridge::test::prefixed;

namespace {

const Record& requireRecord(const ParseResult& result) {
    REQUIRE(result.outcome == ParseOutcome::Record);
    return result.record;
}

template <typename T> const T& fieldsOf(const Record& record) {
    const T* p = std::get_if<T>(&record.fields);
    REQUIRE(p != nullptr);
    return *p;
}

bool isInvalid(std::string_view bare) {
    return parseBare(bare).outcome == ParseOutcome::Invalid;
}

bool isValid(std::string_view bare) {
    return parseBare(bare).outcome == ParseOutcome::Record;
}

// Valid frame-less records for the int-grammar tests: <int> goes into FIN's signed gold field,
// VASSALS' non-negative total, VASSAL's positive rank.
std::string finWith(const std::string& gold) {
    return "CURIA1|7|FIN|" + gold + "|9";
}
std::string vassalsWith(const std::string& total) {
    return "CURIA1|7|VASSALS|" + total + "|0";
}
std::string rankWith(const std::string& rank) {
    return "CURIA1|7|VASSAL|" + rank + "|1|2|3|4|x";
}

const std::string kEuro = "\xE2\x82\xAC";     // 3 bytes
const std::string kPoo = "\xF0\x9F\x92\xA9";  // 4 bytes
const std::string kEacute = "\xC3\xA9";       // 2 bytes

}  // namespace

// ---- step 1: the marker -------------------------------------------------------------------------

TEST_CASE("parser step 1: a line without the marker is not a Curia line") {
    CHECK(parseLine("", false).outcome == ParseOutcome::NotCuria);
    CHECK(parseLine("some engine message", false).outcome == ParseOutcome::NotCuria);
    CHECK(parseLine("prefix CURIA2|7|FIN|1|2", false).outcome == ParseOutcome::NotCuria);
    CHECK(parseLine("prefix curia1|7|FIN|1|2", false).outcome == ParseOutcome::NotCuria);
    CHECK(parseLine("prefix CURIA1 |7|FIN|1|2", false).outcome == ParseOutcome::NotCuria);
    CHECK(parseLine("prefix CURIA1", false).outcome == ParseOutcome::NotCuria);
}

TEST_CASE("parser step 1: the marker at offset 0 is refused") {
    CHECK(parseLine("CURIA1|7|FIN|1|2", false).outcome == ParseOutcome::NotCuria);
    // The first occurrence decides: a later marker does not rescue the line.
    CHECK(parseLine("CURIA1|7|FIN|1|2 CURIA1|7|FIN|1|2", false).outcome == ParseOutcome::NotCuria);
    // One byte of prefix is enough.
    CHECK(parseLine(" CURIA1|7|FIN|1|2", false).outcome == ParseOutcome::Record);
    CHECK(parseLine("x CURIA1|7|FIN|1|2", false).outcome == ParseOutcome::Record);
}

TEST_CASE("parser step 1: the record starts at the first marker") {
    const ParseResult result = parseLine("pre CURIA1|7|HEIR|1|2|1|0|CURIA1|8|FIN|1|2", false);
    const Record& record = requireRecord(result);
    CHECK(record.type == RecordType::Heir);
    CHECK(record.snap == 7);
    CHECK(record.text == "CURIA1|8|FIN|1|2");  // the second marker is just text
}

// ---- step 2: the header
// ---------------------------------------------------------------------------

TEST_CASE("parser step 2: header failures are invalid records") {
    const std::array<const char*, 19> bad = {
        "CURIA1|",        "CURIA1||BEGIN",    "CURIA1|abc|FOO", "CURIA1|7",
        "CURIA1|7|",      "CURIA1|7|fin|1|2", "CURIA1|7|Fin",   "CURIA1|7|1FOO",
        "CURIA1|7|_FOO",  "CURIA1|7|FOO-BAR", "CURIA1|7|FO O",  "CURIA1|-5|FOO",
        "CURIA1|-0|FOO",  "CURIA1|00|FOO",    "CURIA1|+7|FOO",  "CURIA1|2147483648|FOO",
        "CURIA1|1.5|FOO", "CURIA1| 7|FOO",    "CURIA1|7 |FOO",
    };
    for (const char* record : bad) {
        CAPTURE(record);
        const ParseResult result = parseBare(record);
        REQUIRE(result.outcome == ParseOutcome::Invalid);
        CHECK(result.invalid.reason == InvalidReason::Header);
    }
}

TEST_CASE("parser step 2: the snap is reported when it parsed") {
    ParseResult result = parseBare("CURIA1|7|fin|1|2");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    REQUIRE(result.invalid.snap.has_value());
    CHECK(must(result.invalid.snap) == 7);
    CHECK_FALSE(result.invalid.isBegin);

    result = parseBare("CURIA1|abc|FOO");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK_FALSE(result.invalid.snap.has_value());

    result = parseBare("CURIA1|-5|BEGIN|0|1|2|3|4|5|x");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK_FALSE(result.invalid.snap.has_value());
    CHECK_FALSE(result.invalid.isBegin);  // no int snap, so not an unsupported-format candidate
}

TEST_CASE("parser step 2: snap limits") {
    CHECK(isValid("CURIA1|2147483647|FOO"));
    CHECK(isValid("CURIA1|1|FOO"));
    CHECK(isValid("CURIA1|10|FOO"));
    CHECK(isInvalid("CURIA1|2147483648|FOO"));
    CHECK(isInvalid("CURIA1|99999999999|FOO"));
}

// ---- step 3: snap 0
// -------------------------------------------------------------------------------

TEST_CASE("parser step 3: snap 0 is valid only on LOAD") {
    CHECK(isValid("CURIA1|0|LOAD|0.1.0"));
    CHECK(isInvalid("CURIA1|1|LOAD|0.1.0"));
    CHECK(isInvalid("CURIA1|7|LOAD|0.1.0"));
    CHECK(isInvalid("CURIA1|0|BEGIN|0|0.1.0|1066|9|15|1001|x"));
    CHECK(isInvalid("CURIA1|0|END|1"));
    CHECK(isInvalid("CURIA1|0|FOO"));
    CHECK(isInvalid("CURIA1|0|PROBE|a|b"));
    ParseResult result = parseBare("CURIA1|0|BEGIN|0|0.1.0|1066|9|15|1001|x");
    CHECK(result.invalid.reason == InvalidReason::SnapRule);
    REQUIRE(result.invalid.snap.has_value());
    CHECK(must(result.invalid.snap) == 0);
    result = parseBare("CURIA1|3|LOAD|0.1.0");
    CHECK(result.invalid.reason == InvalidReason::SnapRule);
}

// ---- the worked invalid records of section 2
// ------------------------------------------------------

// Spec section 2, row of the worked invalid records: `7|FIN|312` | too few fields
TEST_CASE("invalid row: too few fields") {
    CHECK(isInvalid("CURIA1|7|FIN|312"));
    CHECK(isInvalid("CURIA1|7|FIN"));
}

// Spec section 2, row of the worked invalid records: `7|FIN|312|9|1` | too many fields, no text
// field on `FIN`
TEST_CASE("invalid row: too many fields, no text field on FIN") {
    const ParseResult result = parseBare("CURIA1|7|FIN|312|9|1");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::FieldCount);
}

// Spec section 2, row of the worked invalid records: `7|FIN|312|9\r` inside the line | `\r` not
// directly before `\n`
TEST_CASE("invalid row: a CR inside the line, not directly before the LF") {
    CHECK(isInvalid("CURIA1|7|FIN|312|9\r"));
    CHECK(isInvalid("CURIA1|7|FIN|312\r|9"));
    CHECK(isInvalid("CURIA1|7|FIN|312|\r9"));
}

// Spec section 2, row of the worked invalid records: `7|FIN|007|9` | leading zeros
TEST_CASE("invalid row: leading zeros") {
    CHECK(isInvalid("CURIA1|7|FIN|007|9"));
}

// Spec section 2, row of the worked invalid records: `7|FIN|+5|9` | plus sign
TEST_CASE("invalid row: plus sign") {
    CHECK(isInvalid("CURIA1|7|FIN|+5|9"));
}

// Spec section 2, row of the worked invalid records: `7|FIN||9` | empty int
TEST_CASE("invalid row: empty int") {
    CHECK(isInvalid("CURIA1|7|FIN||9"));
    CHECK(isInvalid("CURIA1|7|FIN|9|"));
}

// Spec section 2, row of the worked invalid records: `7|FIN|1e3|9` | not an int
TEST_CASE("invalid row: not an int") {
    CHECK(isInvalid("CURIA1|7|FIN|1e3|9"));
    CHECK(isInvalid("CURIA1|7|FIN|1.5|9"));
    CHECK(isInvalid("CURIA1|7|FIN|1,000|9"));
    CHECK(isInvalid("CURIA1|7|FIN|0x10|9"));
    CHECK(isInvalid("CURIA1|7|FIN| 5|9"));
    CHECK(isInvalid("CURIA1|7|FIN|5 |9"));
    CHECK(isInvalid("CURIA1|7|FIN|\xD9\xA5|9"));  // an Arabic-Indic digit
    CHECK(isInvalid("CURIA1|7|FIN|\xE2\x88\x95"
                    "5|9"));  // U+2215, not a minus
    CHECK(isInvalid("CURIA1|7|FIN|\xE2\x88\x92"
                    "5|9"));  // U+2212 minus sign
}

// Spec section 2, row of the worked invalid records: `7|END|99999999999999999999` | does not fit
// int32
TEST_CASE("invalid row: does not fit int32") {
    CHECK(isInvalid("CURIA1|7|END|99999999999999999999"));
    CHECK(isInvalid("CURIA1|7|END|2147483648"));
    CHECK(isInvalid("CURIA1|7|END|10000000000"));
}

// Spec section 2, row of the worked invalid records: `7|HEIR|1002|84|1|0` | text delimiter missing
TEST_CASE("invalid row: text delimiter missing (HEIR)") {
    const ParseResult result = parseBare("CURIA1|7|HEIR|1002|84|1|0");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::FieldCount);
}

// Spec section 2, row of the worked invalid records: `7|HEIR|none|` | bytes after `none`
TEST_CASE("invalid row: bytes after none (HEIR)") {
    const ParseResult result = parseBare("CURIA1|7|HEIR|none|");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::NoneForm);
    CHECK(isInvalid("CURIA1|7|HEIR|none|x"));
}

// Spec section 2, row of the worked invalid records: `7|FACTION|1|LEADER|1004` | text delimiter
// missing
TEST_CASE("invalid row: text delimiter missing (FACTION LEADER)") {
    const ParseResult result = parseBare("CURIA1|7|FACTION|1|LEADER|1004");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::FieldCount);
}

// Spec section 2, row of the worked invalid records: `7|CLAIM|1|Weak|4|x` | enum is case-sensitive
TEST_CASE("invalid row: an enum is case-sensitive") {
    CHECK(isInvalid("CURIA1|7|CLAIM|1|Weak|4|x"));
    CHECK(isInvalid("CURIA1|7|CLAIM|1|STRONG|4|x"));
    CHECK(isInvalid("CURIA1|7|CLAIM|1|medium|4|x"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|Against|3|33|100|2"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|other|3|33|100|2"));
}

// Spec section 2, row of the worked invalid records: `7|BEGIN|0|1.0 0|...` | token with a space
TEST_CASE("invalid row: a token with a space") {
    CHECK(isInvalid("CURIA1|7|BEGIN|0|1.0 0|1066|9|15|1001|x"));
    CHECK(isInvalid("CURIA1|0|LOAD|0.1.0 beta"));
    CHECK(isInvalid("CURIA1|0|LOAD|"));
    CHECK(isInvalid("CURIA1|0|LOAD|" + std::string(33, 'a')));
    CHECK(isValid("CURIA1|0|LOAD|" + std::string(32, 'a')));
    CHECK(isInvalid("CURIA1|0|LOAD|a|b"));
}

// Spec section 2, row of the worked invalid records: `7|fin|1|2` | type outside the pattern
TEST_CASE("invalid row: a type outside the pattern") {
    CHECK(isInvalid("CURIA1|7|fin|1|2"));
}

// Spec section 2, row of the worked invalid records: `7|FACTION|1|TITLE|none|` | bytes after `none`
TEST_CASE("invalid row: bytes after none (FACTION TITLE)") {
    const ParseResult result = parseBare("CURIA1|7|FACTION|1|TITLE|none|");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::NoneForm);
}

// Spec section 2, row of the worked invalid records: `7|FACTION|1` | `FACTION` without a sub-kind
TEST_CASE("invalid row: FACTION without a sub-kind") {
    const ParseResult result = parseBare("CURIA1|7|FACTION|1");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::SubKind);
    CHECK(isInvalid("CURIA1|7|FACTION"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|"));
    CHECK(isInvalid("CURIA1|7|WAR|1"));
}

// Spec section 2, row of the worked invalid records: `0|BEGIN|0|0.1.0|1066|9|15|1|x` | snap 0 is
// valid only on `LOAD`
TEST_CASE("invalid row: snap 0 on BEGIN") {
    const ParseResult result = parseBare("CURIA1|0|BEGIN|0|0.1.0|1066|9|15|1|x");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::SnapRule);
}

// Spec section 2, row of the worked invalid records: `7|VASSALS|-0|0` | `-0` in a non-negative
// field
TEST_CASE("invalid row: -0 in a non-negative field") {
    CHECK(isInvalid("CURIA1|7|VASSALS|-0|0"));
    CHECK(isInvalid("CURIA1|7|VASSALS|0|-0"));
    CHECK(isInvalid("CURIA1|7|END|-0"));
    CHECK(isInvalid("CURIA1|-0|FOO"));
}

// Spec section 2, "Valid and notable": `7|HEIR|1002|84|1|0|`, `7|HEIR|none`, `7|FIN|-0|-0`,
// `7|FACTION|2|DISCONTENT|42`
TEST_CASE("valid and notable: the examples of section 2") {
    {
        const ParseResult result = parseBare("CURIA1|7|HEIR|1002|84|1|0|");
        const Record& record = requireRecord(result);
        CHECK(record.text.empty());
        CHECK_FALSE(record.textShortened);
        const auto& heir = fieldsOf<HeirFields>(record);
        CHECK_FALSE(heir.none);
        CHECK(heir.id == 1002);
        CHECK(heir.opinion == 84);
        CHECK(heir.hasLiege);
        CHECK_FALSE(heir.landed);
    }
    {
        const Record record = requireRecord(parseBare("CURIA1|7|HEIR|none"));
        CHECK(fieldsOf<HeirFields>(record).none);
    }
    {
        const Record record = requireRecord(parseBare("CURIA1|7|FIN|-0|-0"));
        const auto& fin = fieldsOf<FinFields>(record);
        CHECK(fin.gold == 0);
        CHECK(fin.income == 0);
    }
    {
        const ParseResult result = parseBare("CURIA1|7|FACTION|2|DISCONTENT|42");
        const Record& record = requireRecord(result);
        CHECK(record.type == RecordType::Unknown);
        CHECK(record.typeName == "FACTION");
        CHECK(record.subKind == "DISCONTENT");
    }
}

// ---- step 4: unknown types
// ---------------------------------------------------------------------------

TEST_CASE("unknown type: ignored but a valid record; header-only validation") {
    const ParseResult result = parseBare("CURIA1|7|FOO|anything at all|\x01\xFF|||");
    const Record& record = requireRecord(result);
    CHECK(record.type == RecordType::Unknown);
    CHECK(record.typeName == "FOO");
    CHECK(record.snap == 7);
    CHECK(record.text.empty());
    CHECK(record.length > 0);
    CHECK(isValid("CURIA1|7|FOO"));
    CHECK(isValid("CURIA1|7|X1_2"));
}

TEST_CASE("unknown type: PROBE is an unknown type, never invalid") {
    CHECK(requireRecord(parseBare("CURIA1|7|PROBE|name|value\x01 bytes")).type ==
          RecordType::Unknown);
    CHECK(requireRecord(parseBare("CURIA1|7|PROBE")).type == RecordType::Unknown);
    // The header rules still apply.
    CHECK(isInvalid("CURIA1|0|PROBE|a"));
    CHECK(isInvalid("CURIA1|x|PROBE|a"));
}

TEST_CASE("unknown type: exempt from the byte-class and oversize rules") {
    const std::string longRecord = "CURIA1|7|FOO|" + std::string(3000, '\x01');
    const Record record = requireRecord(parseBare(longRecord));
    CHECK(record.type == RecordType::Unknown);
    CHECK_FALSE(record.textShortened);
    CHECK(record.length == longRecord.size());
    // An overlong line gives an unknown record as well (the head only).
    const Record fromOverlong = requireRecord(parseBare("CURIA1|7|FOO|bar", true));
    CHECK_FALSE(fromOverlong.textShortened);
}

TEST_CASE("unknown sub-kind: n must be an int of 1 or more and the word must match") {
    CHECK(isValid("CURIA1|7|FACTION|1|DISCONTENT"));
    CHECK(isValid("CURIA1|7|FACTION|1|DISCONTENT|anything \x01 goes|"));
    CHECK(isValid("CURIA1|7|WAR|3|SCORE|55"));
    CHECK(isInvalid("CURIA1|7|FACTION|0|DISCONTENT|42"));
    CHECK(isInvalid("CURIA1|7|FACTION|-1|DISCONTENT|42"));
    CHECK(isInvalid("CURIA1|7|FACTION|x|DISCONTENT|42"));
    CHECK(isInvalid("CURIA1|7|FACTION||DISCONTENT|42"));
    CHECK(isInvalid("CURIA1|7|FACTION|01|DISCONTENT|42"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|discontent|42"));
    CHECK(isInvalid("CURIA1|7|FACTION|1||42"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|1BAD|42"));
    CHECK(isInvalid("CURIA1|7|WAR|1|SC ORE|42"));
    // A sub-kind of the other type is unknown for this type.
    CHECK(requireRecord(parseBare("CURIA1|7|WAR|1|INFO|x")).type == RecordType::Unknown);
    CHECK(requireRecord(parseBare("CURIA1|7|FACTION|1|NAME|x")).type == RecordType::Unknown);
}

// ---- step 5: typed fields for every known type
// ----------------------------------------------------

TEST_CASE("fields: LOAD") {
    const Record record = requireRecord(parseBare("CURIA1|0|LOAD|0.1.0-rc.1+b2"));
    CHECK(record.type == RecordType::Load);
    CHECK(record.snap == 0);
    CHECK(fieldsOf<LoadFields>(record).modVersion == "0.1.0-rc.1+b2");
}

TEST_CASE("fields: BEGIN") {
    const Record record =
        requireRecord(parseBare("CURIA1|7|BEGIN|1|0.1.0|1066|9|15|1001|Test Ruler One"));
    CHECK(record.type == RecordType::Begin);
    const auto& begin = fieldsOf<BeginFields>(record);
    CHECK(begin.mode == 1);
    CHECK(begin.modVersion == "0.1.0");
    CHECK(begin.year == 1066);
    CHECK(begin.month == 9);
    CHECK(begin.day == 15);
    CHECK(begin.rulerId == 1001);
    CHECK(record.text == "Test Ruler One");
}

TEST_CASE("fields: BEGIN ranges") {
    const auto begin = [](const std::string& mode, const std::string& year,
                          const std::string& month, const std::string& day, const std::string& id) {
        return "CURIA1|7|BEGIN|" + mode + "|0.1.0|" + year + "|" + month + "|" + day + "|" + id +
               "|x";
    };
    CHECK(isValid(begin("0", "0", "1", "1", "0")));
    CHECK(isValid(begin("1", "9999", "12", "31", "2147483647")));
    CHECK(isInvalid(begin("2", "1066", "9", "15", "1")));
    CHECK(isInvalid(begin("-1", "1066", "9", "15", "1")));
    CHECK(isInvalid(begin("0", "10000", "9", "15", "1")));
    CHECK(isInvalid(begin("0", "-1", "9", "15", "1")));
    CHECK(isInvalid(begin("0", "1066", "0", "15", "1")));
    CHECK(isInvalid(begin("0", "1066", "13", "15", "1")));
    CHECK(isInvalid(begin("0", "1066", "9", "0", "1")));
    CHECK(isInvalid(begin("0", "1066", "9", "32", "1")));
    CHECK(isInvalid(begin("0", "1066", "9", "15", "-1")));
    // A calendar-impossible date is accepted.
    CHECK(isValid(begin("0", "1066", "2", "31", "1")));
}

TEST_CASE("fields: FIN") {
    const Record record = requireRecord(parseBare("CURIA1|7|FIN|-312|-78"));
    const auto& fin = fieldsOf<FinFields>(record);
    CHECK(fin.gold == -312);
    CHECK(fin.income == -78);
    CHECK(record.text.empty());
}

TEST_CASE("fields: HEIR full form") {
    const Record record = requireRecord(parseBare("CURIA1|7|HEIR|1002|-84|0|1|Test Heir Two"));
    const auto& heir = fieldsOf<HeirFields>(record);
    CHECK_FALSE(heir.none);
    CHECK(heir.id == 1002);
    CHECK(heir.opinion == -84);
    CHECK_FALSE(heir.hasLiege);
    CHECK(heir.landed);
    CHECK(record.text == "Test Heir Two");
    CHECK(isInvalid("CURIA1|7|HEIR|1002|84|2|0|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|-1|84|1|0|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|None"));
    CHECK(isInvalid("CURIA1|7|HEIR|"));
    CHECK(isInvalid("CURIA1|7|HEIR"));
}

TEST_CASE("fields: VASSALS and VASSAL") {
    const Record header = requireRecord(parseBare("CURIA1|7|VASSALS|12|10"));
    CHECK(fieldsOf<VassalsFields>(header).total == 12);
    CHECK(fieldsOf<VassalsFields>(header).listed == 10);
    const Record item =
        requireRecord(parseBare("CURIA1|7|VASSAL|1|1003|120|-55|1523|Test Vassal Three"));
    const auto& v = fieldsOf<VassalFields>(item);
    CHECK(v.rank == 1);
    CHECK(v.id == 1003);
    CHECK(v.gold == 120);
    CHECK(v.opinion == -55);
    CHECK(v.strength == 1523);
    CHECK(item.text == "Test Vassal Three");
    CHECK(isInvalid("CURIA1|7|VASSAL|0|1|2|3|4|x"));   // rank 0
    CHECK(isInvalid("CURIA1|7|VASSAL|1|-1|2|3|4|x"));  // negative id
    CHECK(isInvalid("CURIA1|7|VASSAL|1|1|2|3|-4|x"));  // negative strength
    CHECK(isInvalid("CURIA1|7|VASSAL|1|1|2|3|4"));     // no text delimiter
}

TEST_CASE("fields: WARS and WAR") {
    CHECK(fieldsOf<WarsFields>(requireRecord(parseBare("CURIA1|7|WARS|3"))).total == 3);
    const Record name = requireRecord(parseBare("CURIA1|7|WAR|2|NAME|Test War | Two"));
    CHECK(fieldsOf<WarFields>(name).n == 2);
    CHECK(fieldsOf<WarFields>(name).part == WarPart::Name);
    CHECK(name.text == "Test War | Two");
    const Record attacker =
        requireRecord(parseBare("CURIA1|7|WAR|2|ATTACKER|1004|Test Vassal Four"));
    CHECK(fieldsOf<WarFields>(attacker).part == WarPart::Attacker);
    CHECK(fieldsOf<WarFields>(attacker).id == 1004);
    CHECK(attacker.text == "Test Vassal Four");
    const Record defender = requireRecord(parseBare("CURIA1|7|WAR|2|DEFENDER|1001|"));
    CHECK(fieldsOf<WarFields>(defender).part == WarPart::Defender);
    CHECK(defender.text.empty());
    CHECK(isInvalid("CURIA1|7|WAR|2|NAME"));           // no delimiter
    CHECK(isInvalid("CURIA1|7|WAR|2|ATTACKER|1004"));  // no delimiter
    CHECK(isInvalid("CURIA1|7|WAR|2|ATTACKER|x|name"));
    CHECK(isInvalid("CURIA1|7|WAR|0|NAME|x"));
}

TEST_CASE("fields: CLAIMS and CLAIM") {
    const Record header = requireRecord(parseBare("CURIA1|7|CLAIMS|12|3|10"));
    const auto& c = fieldsOf<ClaimsFields>(header);
    CHECK(c.explicitCount == 12);
    CHECK(c.implicitCount == 3);
    CHECK(c.listed == 10);
    const Record item = requireRecord(parseBare("CURIA1|7|CLAIM|1|strong|5|Test Empire"));
    CHECK(fieldsOf<ClaimFields>(item).kind == ClaimKind::Strong);
    CHECK(fieldsOf<ClaimFields>(item).tier == 5);
    CHECK(requireRecord(parseBare("CURIA1|7|CLAIM|2|weak|1|x")).type == RecordType::Claim);
    CHECK(isInvalid("CURIA1|7|CLAIM|1|weak|0|x"));
    CHECK(isInvalid("CURIA1|7|CLAIM|1|weak|6|x"));
    CHECK(isInvalid("CURIA1|7|CLAIM|0|weak|4|x"));
}

TEST_CASE("fields: FACTIONS and FACTION INFO") {
    const Record header = requireRecord(parseBare("CURIA1|7|FACTIONS|9|1|6"));
    const auto& f = fieldsOf<FactionsFields>(header);
    CHECK(f.against == 9);
    CHECK(f.member);
    CHECK(f.listed == 6);
    CHECK(isInvalid("CURIA1|7|FACTIONS|9|2|6"));
    const Record info = requireRecord(parseBare("CURIA1|7|FACTION|2|INFO|member|7|170|0|12"));
    const auto& i = fieldsOf<FactionInfoFields>(info);
    CHECK(i.n == 2);
    CHECK(i.role == FactionRole::Member);
    CHECK(i.type == 7);
    CHECK(i.power == 170);
    CHECK(i.threshold == 0);
    CHECK(i.members == 12);
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|against|0|1|1|1"));   // type 0
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|against|8|1|1|1"));   // type 8
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|against|3|1|1"));     // too few
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|against|3|1|1|1|"));  // too many
    CHECK(isInvalid("CURIA1|7|FACTION|1|INFO|against|3|-1|1|1"));
}

TEST_CASE("fields: FACTION characters and title, full and none forms") {
    for (const char* sub : {"LEADER", "CLAIMANT", "TARGET"}) {
        CAPTURE(sub);
        const Record full = requireRecord(
            parseBare(std::string("CURIA1|7|FACTION|1|") + sub + "|1004|Test Vassal Four"));
        const auto& fields = fieldsOf<FactionCharFields>(full);
        CHECK_FALSE(fields.none);
        CHECK(fields.id == 1004);
        CHECK(full.text == "Test Vassal Four");
        const Record none =
            requireRecord(parseBare(std::string("CURIA1|7|FACTION|1|") + sub + "|none"));
        CHECK(fieldsOf<FactionCharFields>(none).none);
        CHECK(none.text.empty());
        CHECK(isInvalid(std::string("CURIA1|7|FACTION|1|") + sub + "|none|"));
        CHECK(isInvalid(std::string("CURIA1|7|FACTION|1|") + sub + "|none|x"));
        CHECK(isInvalid(std::string("CURIA1|7|FACTION|1|") + sub + "|"));
        CHECK(isInvalid(std::string("CURIA1|7|FACTION|1|") + sub));
        CHECK(isInvalid(std::string("CURIA1|7|FACTION|1|") + sub + "|-1|x"));
    }
    const Record title = requireRecord(parseBare("CURIA1|7|FACTION|1|TITLE|4|Test Realm"));
    CHECK_FALSE(fieldsOf<FactionTitleFields>(title).none);
    CHECK(fieldsOf<FactionTitleFields>(title).tier == 4);
    CHECK(title.text == "Test Realm");
    CHECK(fieldsOf<FactionTitleFields>(requireRecord(parseBare("CURIA1|7|FACTION|1|TITLE|none")))
              .none);
    CHECK(isInvalid("CURIA1|7|FACTION|1|TITLE|0|x"));
    CHECK(isInvalid("CURIA1|7|FACTION|1|TITLE|6|x"));
}

TEST_CASE("fields: END") {
    CHECK(fieldsOf<EndFields>(requireRecord(parseBare("CURIA1|7|END|16"))).count == 16);
    CHECK(fieldsOf<EndFields>(requireRecord(parseBare("CURIA1|7|END|0"))).count == 0);
    CHECK(isInvalid("CURIA1|7|END"));
    CHECK(isInvalid("CURIA1|7|END|"));
    CHECK(isInvalid("CURIA1|7|END|1|2"));
    CHECK(isInvalid("CURIA1|7|END|-1"));
}

// ---- the int grammar
// -------------------------------------------------------------------------------

TEST_CASE("int grammar: signed fields accept the whole int32 range") {
    CHECK(isValid(finWith("0")));
    CHECK(isValid(finWith("-0")));
    CHECK(isValid(finWith("-1")));
    CHECK(isValid(finWith("2147483647")));
    CHECK(isValid(finWith("-2147483648")));
    CHECK(isInvalid(finWith("2147483648")));
    CHECK(isInvalid(finWith("-2147483649")));
    CHECK(isInvalid(finWith("-")));
    CHECK(isInvalid(finWith("--1")));
    CHECK(isInvalid(finWith("-00")));
    CHECK(isInvalid(finWith("-01")));
    CHECK(isInvalid(finWith("00")));
    CHECK(isInvalid(finWith("01")));
    CHECK(isInvalid(finWith("10000000000")));
    CHECK(isInvalid(finWith("-10000000000")));
    CHECK(isInvalid(finWith("4294967296")));  // 2^32 must not wrap
    CHECK(isInvalid(finWith("4294967297")));
    CHECK(isInvalid(finWith("9999999999")));
}

TEST_CASE("int grammar: -0 is read as 0 in signed fields") {
    const Record record = requireRecord(parseBare(finWith("-0")));
    CHECK(fieldsOf<FinFields>(record).gold == 0);
    const Record vassal = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|-0|-0|4|x"));
    CHECK(fieldsOf<VassalFields>(vassal).gold == 0);
    CHECK(fieldsOf<VassalFields>(vassal).opinion == 0);
    const Record heir = requireRecord(parseBare("CURIA1|7|HEIR|2|-0|1|0|x"));
    CHECK(fieldsOf<HeirFields>(heir).opinion == 0);
}

TEST_CASE("int grammar: non-negative fields") {
    CHECK(isValid(vassalsWith("0")));
    CHECK(isValid(vassalsWith("2147483647")));
    CHECK(isInvalid(vassalsWith("2147483648")));
    CHECK(isInvalid(vassalsWith("-1")));
    CHECK(isInvalid(vassalsWith("-0")));
    CHECK(isInvalid(vassalsWith("00")));
    CHECK(isInvalid(vassalsWith("")));
}

TEST_CASE("int grammar: positive fields (rank, n)") {
    CHECK(isValid(rankWith("1")));
    CHECK(isValid(rankWith("2147483647")));
    CHECK(isInvalid(rankWith("0")));
    CHECK(isInvalid(rankWith("-1")));
    CHECK(isInvalid(rankWith("-0")));
    CHECK(isInvalid(rankWith("2147483648")));
}

TEST_CASE("int grammar: flags are 0 or 1 only") {
    CHECK(isValid("CURIA1|7|HEIR|1|2|0|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|2|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|-1|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|01|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|true|1|x"));
}

// ---- text fields
// ----------------------------------------------------------------------------------

TEST_CASE("text: it is last, may contain the delimiter, and may be empty") {
    Record record = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|3|4|5|a|b|c|6|d"));
    CHECK(record.text == "a|b|c|6|d");
    record = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|3|4|5|"));
    CHECK(record.text.empty());
    record = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|3|4|5||"));
    CHECK(record.text == "|");
    // A game string that looks like a field cannot move another one.
    record = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|3|4|5|99|99|99|99|99"));
    CHECK(fieldsOf<VassalFields>(record).strength == 5);
    CHECK(record.text == "99|99|99|99|99");
}

TEST_CASE("text: any bytes are allowed, raw, before hygiene") {
    const std::string name = std::string("a\x01\x15 \x7F\xFF\xC3\xA9") + "z\r";
    const Record record = requireRecord(parseBare("CURIA1|7|VASSAL|1|2|3|4|5|" + name));
    CHECK(record.text == name);
    CHECK_FALSE(record.textShortened);
}

TEST_CASE("bytes outside the text field must be printable ASCII") {
    CHECK(isInvalid(std::string("CURIA1|7|CLAIM|1|we\x01") + "ak|4|x"));
    CHECK(isInvalid("CURIA1|7|CLAIM|1|weak|4\x7F|x"));
    CHECK(isInvalid("CURIA1|7|BEGIN|0|0.1.\xC3\xA9|1066|9|15|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|\xC3\xA9|1|1|0|x"));
}

// ---- the oversize rule (section 1) --------------------------------------------------------------

namespace {

const std::string kVassalHead = "CURIA1|7|VASSAL|1|2|3|4|5|";

// A record with exactly `total` bytes: the head plus a name of filler.
std::string vassalOfSize(std::size_t total) {
    return kVassalHead + std::string(total - kVassalHead.size(), 'n');
}

}  // namespace

TEST_CASE("oversize: 1024 bytes is not oversize, 1025 is") {
    Record record = requireRecord(parseBare(vassalOfSize(1024)));
    CHECK_FALSE(record.textShortened);
    CHECK(record.length == 1024);
    CHECK(record.text.size() == 1024 - kVassalHead.size());

    record = requireRecord(parseBare(vassalOfSize(1025)));
    CHECK(record.textShortened);
    CHECK(record.length == 1024);
    CHECK(record.text.size() == 1024 - kVassalHead.size());
    CHECK(fieldsOf<VassalFields>(record).strength == 5);
}

TEST_CASE("oversize: an oversize text record of any size is cut to 1024 bytes") {
    const Record record = requireRecord(parseBare(vassalOfSize(4000)));
    CHECK(record.textShortened);
    CHECK(record.length == 1024);
    CHECK(record.text == std::string(1024 - kVassalHead.size(), 'n'));
}

TEST_CASE("oversize: the cut never leaves half a character, at every offset") {
    // Slide the character boundary across the cut by changing the number of ASCII bytes before the
    // multi-byte characters; for each, the text must be valid UTF-8 and at most 3 bytes short.
    const std::array<std::string, 3> chars = {kEacute, kEuro, kPoo};
    for (const std::string& ch : chars) {
        for (std::size_t shift = 0; shift < 8; ++shift) {
            CAPTURE(ch.size());
            CAPTURE(shift);
            std::string name(shift, 'a');
            for (int i = 0; i < 600; ++i) {
                name += ch;
            }
            const std::string bare = kVassalHead + name;
            const Record record = requireRecord(parseBare(bare));
            const std::size_t room = 1024 - kVassalHead.size();
            REQUIRE(record.textShortened);
            CHECK(record.text == curia::bridge::repairUtf8(record.text));  // valid UTF-8
            CHECK(record.text.size() <= room);
            CHECK(record.text.size() + 3 >= room);
            CHECK(name.compare(0, record.text.size(), record.text) == 0);  // a prefix of the name
            CHECK(record.length == kVassalHead.size() + record.text.size());
            // Nothing complete was thrown away: the next character would not have fitted.
            const std::size_t charLength = ch.size();
            CHECK(record.text.size() + charLength > room);
        }
    }
}

TEST_CASE("oversize: other invalid bytes at the cut are kept, only a started sequence goes") {
    const std::size_t room = 1024 - kVassalHead.size();
    std::string name(room - 1, 'a');
    name += '\xFF';  // the last byte kept is not the start of a UTF-8 sequence
    name += "tail";
    Record record = requireRecord(parseBare(kVassalHead + name));
    CHECK(record.textShortened);
    CHECK(record.text.size() == room);
    CHECK(record.text.back() == '\xFF');

    // A stray continuation byte at the cut is kept as well.
    name = std::string(room - 1, 'a') + '\x80' + "tail";
    record = requireRecord(parseBare(kVassalHead + name));
    CHECK(record.text.size() == room);

    // A lone lead byte at the end of the cut goes; the byte before it stays.
    name = std::string(room - 1, 'a') + '\xE2' + "tail";
    record = requireRecord(parseBare(kVassalHead + name));
    CHECK(record.text.size() == room - 1);
    CHECK(record.text.back() == 'a');
}

TEST_CASE("trimIncompleteUtf8Tail: every prefix of a multi-byte sequence") {
    for (const std::string& ch : {kEacute, kEuro, kPoo}) {
        const std::string text = "ab" + ch;
        for (std::size_t keep = 2; keep <= text.size(); ++keep) {
            const std::string_view cut = std::string_view(text).substr(0, keep);
            const std::string_view trimmed = trimIncompleteUtf8Tail(cut);
            if (keep == text.size()) {
                CHECK(trimmed.size() == keep);  // complete
            } else {
                CHECK(trimmed == "ab");
            }
        }
    }
    CHECK(trimIncompleteUtf8Tail("").empty());
    CHECK(trimIncompleteUtf8Tail("abc") == "abc");
    CHECK(trimIncompleteUtf8Tail("\x80") == "\x80");  // a lone continuation byte stays
    CHECK(trimIncompleteUtf8Tail("a\x80\x80\x80") == "a\x80\x80\x80");
    CHECK(trimIncompleteUtf8Tail("\xC0") == "\xC0");  // not a lead byte
    CHECK(trimIncompleteUtf8Tail("\xF5") == "\xF5");
    CHECK(trimIncompleteUtf8Tail("\xFF") == "\xFF");
}

TEST_CASE("oversize: a long record is valid only if the text field is what makes it long") {
    // The fixed fields of every type are short, so a valid oversize record always has its fixed
    // fields and the delimiter inside the first 1024 bytes.
    const Record ok = requireRecord(parseBare("CURIA1|7|BEGIN|0|" + std::string(32, 'v') +
                                              "|1066|9|15|1001|" + std::string(2000, 'n')));
    CHECK(ok.textShortened);
    // A record that is long because of a fixed field (a run of digits that never ends) cannot be
    // saved by the cut: the field is not an int, or the delimiter is not within the first 1024.
    CHECK(isInvalid("CURIA1|7|END|" + std::string(2000, '1')));
    CHECK(isInvalid("CURIA1|7|VASSAL|1|2|3|4|" + std::string(2000, '5')));
    CHECK(isInvalid("CURIA1|7|VASSAL|1|2|3|4|" + std::string(2000, '5') + "|name"));
    CHECK(isInvalid("CURIA1|0|LOAD|" + std::string(2000, 'a')));
    // The same, from an overlong line.
    CHECK(parseBare("CURIA1|7|VASSAL|1|2|3|4|" + std::string(2000, '5'), true).outcome ==
          ParseOutcome::Invalid);
}

TEST_CASE("oversize: a record without a text field is invalid when oversize") {
    // Valid record, then an overlong line cuts nothing visible: an overlong FIN is invalid.
    ParseResult result = parseBare("CURIA1|7|FIN|1|2", true);
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.reason == InvalidReason::Oversize);
    result = parseBare("CURIA1|7|HEIR|none", true);
    CHECK(result.outcome == ParseOutcome::Invalid);
    result = parseBare("CURIA1|7|FACTION|1|LEADER|none", true);
    CHECK(result.outcome == ParseOutcome::Invalid);
    result = parseBare("CURIA1|7|FACTION|1|TITLE|none", true);
    CHECK(result.outcome == ParseOutcome::Invalid);
    // A long record without text: too many bytes after the fields.
    CHECK(isInvalid("CURIA1|7|FIN|1|2" + std::string(2000, ' ')));
    CHECK(isInvalid("CURIA1|7|HEIR|none" + std::string(2000, ' ')));
}

TEST_CASE("oversize: a record from an overlong line counts as oversize even when short") {
    const ParseResult result = parseBare("CURIA1|7|VASSAL|1|2|3|4|5|Short Name", true);
    const Record& record = requireRecord(result);
    CHECK(record.textShortened);
    CHECK(record.text == "Short Name");
    CHECK(record.length == std::string("CURIA1|7|VASSAL|1|2|3|4|5|Short Name").size());
    // An overlong line whose head ends inside the fixed fields is invalid.
    CHECK(parseBare("CURIA1|7|VASSAL|1|2|3|4", true).outcome == ParseOutcome::Invalid);
    CHECK(parseBare("CURIA1|7|VASSAL|1|2|3|4|5", true).outcome == ParseOutcome::Invalid);
}

TEST_CASE("oversize: the head of an overlong line is cut like any oversize record") {
    // A 4000-byte head: shortened to 1024, text a prefix of the name.
    const std::string name(3900, 'q');
    const Record record = requireRecord(parseBare(kVassalHead + name, true));
    CHECK(record.textShortened);
    CHECK(record.length == 1024);
    // An overlong head that ends in half a character loses it.
    const std::string half = kVassalHead + "ab" + std::string("\xE2\x82");
    const Record trimmed = requireRecord(parseBare(half, true));
    CHECK(trimmed.text == "ab");
}

TEST_CASE("oversize: BEGIN with a long ruler name is shortened") {
    const Record record =
        requireRecord(parseBare("CURIA1|7|BEGIN|0|0.1.0|1066|9|15|1001|" + std::string(1100, 'r')));
    CHECK(record.textShortened);
    CHECK(record.length == 1024);
    CHECK(fieldsOf<BeginFields>(record).rulerId == 1001);
}

// ---- invalid BEGIN reports (section 3)
// ------------------------------------------------------------

TEST_CASE("invalid BEGIN: snap, isBegin and the leading fields") {
    ParseResult result = parseBare("CURIA1|7|BEGIN|1066.9.15|0.1.0|Test Ruler One|1001|extra");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.isBegin);
    REQUIRE(result.invalid.snap.has_value());
    CHECK(must(result.invalid.snap) == 7);
    CHECK(result.invalid.leadingFields == "1066.9.15|0.1.0|Test Ruler One");

    result = parseBare("CURIA1|7|BEGIN");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.isBegin);
    CHECK(result.invalid.leadingFields.empty());

    result = parseBare("CURIA1|7|BEGIN|only");
    CHECK(result.invalid.leadingFields == "only");

    result = parseBare("CURIA1|7|BEGIN|a|b");
    CHECK(result.invalid.leadingFields == "a|b");

    result = parseBare("CURIA1|7|BEGIN||");
    CHECK(result.invalid.leadingFields == "|");
}

TEST_CASE("invalid BEGIN: the report is at most 64 bytes with control bytes shown as ?") {
    ParseResult result = parseBare("CURIA1|7|BEGIN|" + std::string(100, 'x') + "|b|c");
    REQUIRE(result.outcome == ParseOutcome::Invalid);
    CHECK(result.invalid.leadingFields == std::string(64, 'x'));

    result = parseBare(std::string("CURIA1|7|BEGIN|a\x01") + "b|c\r|d\x7F|e");
    CHECK(result.invalid.leadingFields == "a?b|c?|d?");

    // The cut does not split a UTF-8 sequence.
    result = parseBare("CURIA1|7|BEGIN|" + std::string(63, 'x') + kEuro);
    CHECK(result.invalid.leadingFields == std::string(63, 'x'));
}

TEST_CASE("invalid BEGIN: not a candidate for the report without an int snap of 0 or more") {
    CHECK_FALSE(parseBare("CURIA1|x|BEGIN|a").invalid.isBegin);
    CHECK_FALSE(parseBare("CURIA1|7|begin|a").invalid.isBegin);
    CHECK_FALSE(parseBare("CURIA1|7|FIN|a").invalid.isBegin);
    // The parser reports snap 0 as parsed; the assembler decides what a snap of 0 means.
    CHECK(parseBare("CURIA1|0|BEGIN|a").invalid.snap.value_or(-1) == 0);
}

TEST_CASE("parser never reads out of bounds on truncated input") {
    const std::string record = "CURIA1|7|FACTION|1|INFO|against|3|33|100|2";
    const std::size_t typeEnd = std::string("CURIA1|7|FACTION|1|INFO").size();
    for (std::size_t n = 0; n <= record.size(); ++n) {
        const ParseResult result =
            parseLine(prefixed(std::string_view(record).substr(0, n)), false);
        if (n == record.size()) {
            CHECK(result.outcome == ParseOutcome::Record);
        } else if (n >= typeEnd) {
            // Once type and sub-kind are complete, every strict prefix lacks a field (a prefix
            // inside the type word or the sub-kind word is a shorter, unknown one: a valid record).
            CHECK(result.outcome == ParseOutcome::Invalid);
        }
    }
}

// ---- ranges of every field kind (review findings) ---------------------------------------------

namespace {

std::string withValue(std::string text, const std::string& value) {
    const std::size_t at = text.find('@');
    REQUIRE(at != std::string::npos);
    text.replace(at, 1, value);
    return text;
}

}  // namespace

TEST_CASE("ranges: every non-negative field rejects -1, -0, 2147483648 and an empty value") {
    const std::vector<std::string> templates = {
        "CURIA1|7|BEGIN|0|0.1.0|1066|9|15|@|x",
        "CURIA1|7|HEIR|@|0|1|1|x",
        "CURIA1|7|VASSALS|@|0",
        "CURIA1|7|VASSALS|0|@",
        "CURIA1|7|VASSAL|1|@|0|0|0|x",
        "CURIA1|7|VASSAL|1|1|0|0|@|x",
        "CURIA1|7|WARS|@",
        "CURIA1|7|WAR|1|ATTACKER|@|x",
        "CURIA1|7|WAR|1|DEFENDER|@|x",
        "CURIA1|7|CLAIMS|@|0|0",
        "CURIA1|7|CLAIMS|0|@|0",
        "CURIA1|7|CLAIMS|0|0|@",
        "CURIA1|7|FACTIONS|@|0|0",
        "CURIA1|7|FACTIONS|0|0|@",
        "CURIA1|7|FACTION|1|INFO|against|3|@|1|1",
        "CURIA1|7|FACTION|1|INFO|against|3|1|@|1",
        "CURIA1|7|FACTION|1|INFO|against|3|1|1|@",
        "CURIA1|7|FACTION|1|LEADER|@|x",
        "CURIA1|7|FACTION|1|CLAIMANT|@|x",
        "CURIA1|7|FACTION|1|TARGET|@|x",
        "CURIA1|7|END|@",
    };
    for (const std::string& t : templates) {
        CAPTURE(t);
        CHECK(isValid(withValue(t, "0")));
        CHECK(isValid(withValue(t, "2147483647")));
        CHECK(isInvalid(withValue(t, "-1")));
        CHECK(isInvalid(withValue(t, "-0")));
        CHECK(isInvalid(withValue(t, "2147483648")));
        CHECK(isInvalid(withValue(t, "")));
        CHECK(isInvalid(withValue(t, "01")));
        CHECK(isInvalid(withValue(t, "+1")));
    }
}

TEST_CASE("ranges: the signed fields take -1 and -0 but not a value outside int32") {
    const std::vector<std::string> templates = {
        "CURIA1|7|FIN|@|0",
        "CURIA1|7|FIN|0|@",
        "CURIA1|7|HEIR|1|@|1|1|x",
        "CURIA1|7|VASSAL|1|1|@|0|0|x",
        "CURIA1|7|VASSAL|1|1|0|@|0|x",
    };
    for (const std::string& t : templates) {
        CAPTURE(t);
        CHECK(isValid(withValue(t, "-1")));
        CHECK(isValid(withValue(t, "-0")));
        CHECK(isValid(withValue(t, "-2147483648")));
        CHECK(isValid(withValue(t, "2147483647")));
        CHECK(isInvalid(withValue(t, "-2147483649")));
        CHECK(isInvalid(withValue(t, "2147483648")));
        CHECK(isInvalid(withValue(t, "")));
        CHECK(isInvalid(withValue(t, "-")));
    }
}

TEST_CASE("ranges: the 0/1 flags reject 2 in every position") {
    CHECK(isValid("CURIA1|7|HEIR|1|2|1|1|x"));
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|1|2|x"));               // landed
    CHECK(isInvalid("CURIA1|7|HEIR|1|2|2|1|x"));               // has_liege
    CHECK(isInvalid("CURIA1|7|FACTIONS|0|2|0"));               // member
    CHECK(isInvalid("CURIA1|7|BEGIN|2|0.1.0|1066|9|15|1|x"));  // mode
}

TEST_CASE("token: an underscore is allowed in a version, a space is not") {
    CHECK(isValid("CURIA1|0|LOAD|0.1_rc"));
    CHECK(isValid("CURIA1|0|LOAD|0.1.0+build-5"));
    CHECK(isInvalid("CURIA1|0|LOAD|0.1 rc"));
    CHECK(isInvalid("CURIA1|0|LOAD|0.1/rc"));
    CHECK(isInvalid("CURIA1|0|LOAD|"));
}
