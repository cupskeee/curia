// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

// Line-level parser of the CURIA1 wire format (docs/wire-format.md sections 1, 2 and 4). Pure: one
// physical line in, one ParseResult out. It applies the marker search, the order of evaluation, the
// field grammar and the oversize rule, but not the text hygiene and not the frame rules.
namespace curia::bridge {

inline constexpr std::string_view kMarker = "CURIA1|";
// A record longer than this (or taken from an overlong line) is oversize (section 1).
inline constexpr std::size_t kMaxRecordBytes = 1024;
// Length of the leading-fields report of an invalid BEGIN (section 3, state machine table).
inline constexpr std::size_t kMaxLeadingFieldsBytes = 64;

enum class RecordType {
    Load,
    Begin,
    Fin,
    Heir,
    Vassals,
    Vassal,
    Wars,
    War,
    Claims,
    Claim,
    Factions,
    Faction,
    End,
    // A type outside the table of section 4, PROBE, or a FACTION or WAR with an unknown sub-kind.
    Unknown,
};

enum class WarPart { Name, Attacker, Defender };
enum class FactionPart { Info, Leader, Claimant, Title, Target };
enum class ClaimKind { Strong, Weak };
enum class FactionRole { Against, Member };

struct LoadFields {
    std::string modVersion;
};

// The text of a BEGIN is the ruler's name.
struct BeginFields {
    std::int32_t mode = 0;
    std::string modVersion;
    std::int32_t year = 0;
    std::int32_t month = 0;
    std::int32_t day = 0;
    std::int32_t rulerId = 0;
};

struct FinFields {
    std::int32_t gold = 0;
    std::int32_t income = 0;  // tenths of a gold piece
};

// The text of a full HEIR is the heir's name; the `none` form has no other field.
struct HeirFields {
    bool none = false;
    std::int32_t id = 0;
    std::int32_t opinion = 0;
    bool hasLiege = false;
    bool landed = false;
};

struct VassalsFields {
    std::int32_t total = 0;
    std::int32_t listed = 0;
};

struct VassalFields {
    std::int32_t rank = 0;
    std::int32_t id = 0;
    std::int32_t gold = 0;
    std::int32_t opinion = 0;
    std::int32_t strength = 0;
};

struct WarsFields {
    std::int32_t total = 0;
};

// NAME carries only text (id stays 0); ATTACKER and DEFENDER carry an id and the name as text.
struct WarFields {
    std::int32_t n = 0;
    WarPart part = WarPart::Name;
    std::int32_t id = 0;
};

struct ClaimsFields {
    std::int32_t explicitCount = 0;
    std::int32_t implicitCount = 0;
    std::int32_t listed = 0;
};

struct ClaimFields {
    std::int32_t n = 0;
    ClaimKind kind = ClaimKind::Weak;
    std::int32_t tier = 0;
};

struct FactionsFields {
    std::int32_t against = 0;
    bool member = false;
    std::int32_t listed = 0;
};

struct FactionInfoFields {
    std::int32_t n = 0;
    FactionRole role = FactionRole::Against;
    std::int32_t type = 0;
    std::int32_t power = 0;
    std::int32_t threshold = 0;
    std::int32_t members = 0;
};

// LEADER, CLAIMANT and TARGET: `none`, or an id with the name as text.
struct FactionCharFields {
    std::int32_t n = 0;
    FactionPart part = FactionPart::Leader;
    bool none = false;
    std::int32_t id = 0;
};

// TITLE: `none`, or a tier with the title as text.
struct FactionTitleFields {
    std::int32_t n = 0;
    bool none = false;
    std::int32_t tier = 0;
};

struct EndFields {
    std::int32_t count = 0;
};

using Fields =
    std::variant<std::monostate, LoadFields, BeginFields, FinFields, HeirFields, VassalsFields,
                 VassalFields, WarsFields, WarFields, ClaimsFields, ClaimFields, FactionsFields,
                 FactionInfoFields, FactionCharFields, FactionTitleFields, EndFields>;

struct Record {
    std::int32_t snap = 0;
    RecordType type = RecordType::Unknown;
    // The TYPE word as written (also for known types).
    std::string typeName;
    // The sub-kind word of a FACTION or WAR record, else empty.
    std::string subKind;
    // Empty (std::monostate) for an unknown type.
    Fields fields;
    // The text field, raw: after the oversize cut, before hygiene. Empty if the line type has none.
    std::string text;
    // The text field was cut by the oversize rule (section 1).
    bool textShortened = false;
    // Bytes from the marker to the end of the record, after the oversize cut (the unit of the
    // frame's byte cap).
    std::size_t length = 0;
};

enum class InvalidReason {
    // The snap or the TYPE word does not parse.
    Header,
    // Snap 0 on a type other than LOAD, or LOAD with a snap of 1 or more.
    SnapRule,
    // A FACTION or WAR without a valid n or sub-kind word.
    SubKind,
    // Too few or too many fields, or a missing text delimiter.
    FieldCount,
    // A field breaks its grammar, range, enum or token rule (this includes non-ASCII bytes outside
    // a text field).
    FieldValue,
    // Bytes after `none`.
    NoneForm,
    // An oversize record that cannot be kept (no text field, or the fixed fields are not complete
    // within the first 1,024 bytes).
    Oversize,
};

struct InvalidRecord {
    InvalidReason reason = InvalidReason::Header;
    // The snap, when it parsed as an int in 0 .. INT32_MAX.
    std::optional<std::int32_t> snap;
    // The TYPE word is BEGIN and the snap parsed.
    bool isBegin = false;
    // For a BEGIN: the raw text of the first three fields after the type, joined by '|', at most 64
    // bytes, control bytes shown as '?'. Empty otherwise.
    std::string leadingFields;
    std::string detail;
};

enum class ParseOutcome { NotCuria, Invalid, Record };

struct ParseResult {
    ParseOutcome outcome = ParseOutcome::NotCuria;
    Record record;          // valid if outcome is Record
    InvalidRecord invalid;  // valid if outcome is Invalid
};

// Parses one physical line (as produced by LineSplitter). `overlong` says the line was cut to its
// head. Never throws, never reads out of bounds.
ParseResult parseLine(std::string_view line, bool overlong);

// Drops up to 3 trailing bytes that start a UTF-8 sequence whose continuation bytes are not all
// present (the cut rule of section 1). Other invalid bytes are kept.
std::string_view trimIncompleteUtf8Tail(std::string_view s) noexcept;

}  // namespace curia::bridge
