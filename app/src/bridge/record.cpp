// SPDX-License-Identifier: MIT
#include "bridge/record.h"

#include <algorithm>
#include <array>
#include <limits>
#include <span>
#include <utility>

namespace curia::bridge {

namespace {

constexpr std::int32_t kIntMin = std::numeric_limits<std::int32_t>::min();
constexpr std::int32_t kIntMax = std::numeric_limits<std::int32_t>::max();
constexpr std::size_t kNoSubKind = static_cast<std::size_t>(-1);

struct Failure {
    InvalidReason reason = InvalidReason::FieldValue;
    std::string detail;
};

bool fail(Failure& failure, InvalidReason reason, std::string_view name, std::string_view why) {
    failure.reason = reason;
    failure.detail.assign(name);
    failure.detail.append(": ");
    failure.detail.append(why);
    return false;
}

// Section 2, "int": `0`, or an optional `-` and [1-9][0-9]{0,9}, fitting int32. `-0` is accepted
// only when allowNegativeZero is set (signed fields) and reads as 0.
bool parseInt32(std::string_view s, bool allowNegativeZero, std::int32_t& out) noexcept {
    if (s.empty()) {
        return false;
    }
    std::size_t i = 0;
    const bool negative = s[0] == '-';
    if (negative) {
        i = 1;
    }
    if (i == s.size()) {
        return false;
    }
    if (s[i] == '0') {
        if (i + 1 != s.size()) {
            return false;  // leading zero
        }
        if (negative && !allowNegativeZero) {
            return false;
        }
        out = 0;
        return true;
    }
    if (s.size() - i > 10) {
        return false;
    }
    std::int64_t value = 0;
    for (; i < s.size(); ++i) {
        const char c = s[i];
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    if (negative) {
        value = -value;
    }
    if (value < kIntMin || value > kIntMax) {
        return false;
    }
    out = static_cast<std::int32_t>(value);
    return true;
}

bool isTypeWord(std::string_view s) noexcept {
    if (s.empty() || s[0] < 'A' || s[0] > 'Z') {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    });
}

bool isToken(std::string_view s) noexcept {
    if (s.empty() || s.size() > 32) {
        return false;
    }
    return std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
               c == '.' || c == '_' || c == '+' || c == '-';
    });
}

RecordType lookupType(std::string_view word) noexcept {
    struct Entry {
        std::string_view word;
        RecordType type;
    };
    static constexpr std::array<Entry, 13> kTable = {{
        {"LOAD", RecordType::Load},
        {"BEGIN", RecordType::Begin},
        {"FIN", RecordType::Fin},
        {"HEIR", RecordType::Heir},
        {"VASSALS", RecordType::Vassals},
        {"VASSAL", RecordType::Vassal},
        {"WARS", RecordType::Wars},
        {"WAR", RecordType::War},
        {"CLAIMS", RecordType::Claims},
        {"CLAIM", RecordType::Claim},
        {"FACTIONS", RecordType::Factions},
        {"FACTION", RecordType::Faction},
        {"END", RecordType::End},
    }};
    for (const Entry& entry : kTable) {
        if (entry.word == word) {
            return entry.type;
        }
    }
    return RecordType::Unknown;  // includes PROBE
}

std::size_t indexOfWord(std::span<const std::string_view> words, std::string_view word) noexcept {
    std::size_t index = 0;
    for (const std::string_view candidate : words) {
        if (candidate == word) {
            return index;
        }
        ++index;
    }
    return kNoSubKind;
}

constexpr std::array<std::string_view, 3> kWarWords = {"NAME", "ATTACKER", "DEFENDER"};
constexpr std::array<std::string_view, 5> kFactionWords = {"INFO", "LEADER", "CLAIMANT", "TITLE",
                                                           "TARGET"};
constexpr std::array<std::string_view, 2> kClaimKinds = {"strong", "weak"};
constexpr std::array<std::string_view, 2> kRoles = {"against", "member"};

// Reads '|'-delimited fields of one record. A field runs to the next '|' or the end.
class Reader {
public:
    Reader(std::string_view record, std::size_t pos) : s_(record), pos_(pos) {}

    [[nodiscard]] bool atEnd() const noexcept { return pos_ >= s_.size(); }

    // The next fixed field; false if the record ends first.
    bool next(std::string_view& out) noexcept {
        if (pos_ >= s_.size() || s_[pos_] != '|') {
            return false;
        }
        ++pos_;
        const std::size_t bar = s_.find('|', pos_);
        const std::size_t end = bar == std::string_view::npos ? s_.size() : bar;
        out = s_.substr(pos_, end - pos_);
        pos_ = end;
        return true;
    }

    // The text field: the delimiter, then everything up to the end (which may contain '|').
    bool text(std::string_view& out) noexcept {
        if (pos_ >= s_.size() || s_[pos_] != '|') {
            return false;
        }
        out = s_.substr(pos_ + 1);
        pos_ = s_.size();
        return true;
    }

private:
    std::string_view s_;
    std::size_t pos_;
};

bool intField(std::string_view s, Failure& f, std::string_view name, std::int32_t lo,
              std::int32_t hi, bool allowNegativeZero, std::int32_t& out) {
    std::int32_t value = 0;
    if (!parseInt32(s, allowNegativeZero, value)) {
        return fail(f, InvalidReason::FieldValue, name, "not a valid integer");
    }
    if (value < lo || value > hi) {
        return fail(f, InvalidReason::FieldValue, name, "out of range");
    }
    out = value;
    return true;
}

bool readRange(Reader& r, Failure& f, std::string_view name, std::int32_t lo, std::int32_t hi,
               bool allowNegativeZero, std::int32_t& out) {
    std::string_view s;
    if (!r.next(s)) {
        return fail(f, InvalidReason::FieldCount, name, "missing field");
    }
    return intField(s, f, name, lo, hi, allowNegativeZero, out);
}

bool readSigned(Reader& r, Failure& f, std::string_view name, std::int32_t& out) {
    return readRange(r, f, name, kIntMin, kIntMax, true, out);
}

bool readNonNegative(Reader& r, Failure& f, std::string_view name, std::int32_t& out) {
    return readRange(r, f, name, 0, kIntMax, false, out);
}

bool readPositive(Reader& r, Failure& f, std::string_view name, std::int32_t& out) {
    return readRange(r, f, name, 1, kIntMax, false, out);
}

bool readFlag(Reader& r, Failure& f, std::string_view name, bool& out) {
    std::int32_t value = 0;
    if (!readRange(r, f, name, 0, 1, false, value)) {
        return false;
    }
    out = value == 1;
    return true;
}

bool readToken(Reader& r, Failure& f, std::string_view name, std::string& out) {
    std::string_view s;
    if (!r.next(s)) {
        return fail(f, InvalidReason::FieldCount, name, "missing field");
    }
    if (!isToken(s)) {
        return fail(f, InvalidReason::FieldValue, name, "not a token");
    }
    out.assign(s);
    return true;
}

bool readEnum(Reader& r, Failure& f, std::string_view name, std::span<const std::string_view> words,
              std::size_t& index) {
    std::string_view s;
    if (!r.next(s)) {
        return fail(f, InvalidReason::FieldCount, name, "missing field");
    }
    index = indexOfWord(words, s);
    if (index == kNoSubKind) {
        return fail(f, InvalidReason::FieldValue, name, "not one of the allowed words");
    }
    return true;
}

bool readText(Reader& r, Failure& f, Record& record) {
    std::string_view s;
    if (!r.text(s)) {
        return fail(f, InvalidReason::FieldCount, "text", "delimiter or field missing");
    }
    record.text.assign(s);
    return true;
}

bool expectEnd(const Reader& r, Failure& f) {
    if (!r.atEnd()) {
        return fail(f, InvalidReason::FieldCount, "record", "too many fields");
    }
    return true;
}

// `none` or a full form: reads the first field and tells which form it is. For `none` the record
// must end there.
bool readNoneOrFirst(Reader& r, Failure& f, std::string_view name, bool& none,
                     std::string_view& first) {
    if (!r.next(first)) {
        return fail(f, InvalidReason::FieldCount, name, "missing field");
    }
    none = first == "none";
    if (none && !r.atEnd()) {
        return fail(f, InvalidReason::NoneForm, name, "bytes after none");
    }
    return true;
}

// Parses the fields of a known type after the TYPE word into `record`. `hasText` tells whether the
// parsed form has a text field (the oversize rule needs it). For WAR and FACTION the n and the
// sub-kind word are read here again; parseLine has already checked them on the uncut record.
bool parseBody(RecordType type, Reader& r, Failure& f, Record& record, bool& hasText) {
    hasText = false;
    switch (type) {
    case RecordType::Load: {
        LoadFields v;
        if (!readToken(r, f, "mod_version", v.modVersion) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = std::move(v);
        return true;
    }
    case RecordType::Begin: {
        BeginFields v;
        if (!readRange(r, f, "mode", 0, 1, false, v.mode) ||
            !readToken(r, f, "mod_version", v.modVersion) ||
            !readRange(r, f, "year", 0, 9999, false, v.year) ||
            !readRange(r, f, "month", 1, 12, false, v.month) ||
            !readRange(r, f, "day", 1, 31, false, v.day) ||
            !readNonNegative(r, f, "ruler_id", v.rulerId) || !readText(r, f, record)) {
            return false;
        }
        hasText = true;
        record.fields = std::move(v);
        return true;
    }
    case RecordType::Fin: {
        FinFields v;
        if (!readSigned(r, f, "gold", v.gold) || !readSigned(r, f, "income", v.income) ||
            !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::Heir: {
        HeirFields v;
        std::string_view first;
        bool none = false;
        if (!readNoneOrFirst(r, f, "heir", none, first)) {
            return false;
        }
        if (none) {
            v.none = true;
            record.fields = v;
            return true;
        }
        if (!intField(first, f, "id", 0, kIntMax, false, v.id) ||
            !readSigned(r, f, "opinion", v.opinion) || !readFlag(r, f, "has_liege", v.hasLiege) ||
            !readFlag(r, f, "landed", v.landed) || !readText(r, f, record)) {
            return false;
        }
        hasText = true;
        record.fields = v;
        return true;
    }
    case RecordType::Vassals: {
        VassalsFields v;
        if (!readNonNegative(r, f, "total", v.total) ||
            !readNonNegative(r, f, "listed", v.listed) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::Vassal: {
        VassalFields v;
        if (!readPositive(r, f, "rank", v.rank) || !readNonNegative(r, f, "id", v.id) ||
            !readSigned(r, f, "gold", v.gold) || !readSigned(r, f, "opinion", v.opinion) ||
            !readNonNegative(r, f, "strength", v.strength) || !readText(r, f, record)) {
            return false;
        }
        hasText = true;
        record.fields = v;
        return true;
    }
    case RecordType::Wars: {
        WarsFields v;
        if (!readNonNegative(r, f, "total", v.total) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::War: {
        WarFields v;
        std::size_t part = 0;
        if (!readPositive(r, f, "n", v.n) || !readEnum(r, f, "sub-kind", kWarWords, part)) {
            return false;
        }
        v.part = static_cast<WarPart>(part);  // the enum follows the order of kWarWords
        if (v.part != WarPart::Name && !readNonNegative(r, f, "id", v.id)) {
            return false;
        }
        if (!readText(r, f, record)) {
            return false;
        }
        hasText = true;
        record.fields = v;
        return true;
    }
    case RecordType::Claims: {
        ClaimsFields v;
        if (!readNonNegative(r, f, "explicit", v.explicitCount) ||
            !readNonNegative(r, f, "implicit", v.implicitCount) ||
            !readNonNegative(r, f, "listed", v.listed) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::Claim: {
        ClaimFields v;
        std::size_t kind = 0;
        if (!readPositive(r, f, "n", v.n) || !readEnum(r, f, "kind", kClaimKinds, kind) ||
            !readRange(r, f, "tier", 1, 5, false, v.tier) || !readText(r, f, record)) {
            return false;
        }
        v.kind = kind == 0 ? ClaimKind::Strong : ClaimKind::Weak;
        hasText = true;
        record.fields = v;
        return true;
    }
    case RecordType::Factions: {
        FactionsFields v;
        if (!readNonNegative(r, f, "against", v.against) || !readFlag(r, f, "member", v.member) ||
            !readNonNegative(r, f, "listed", v.listed) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::Faction: {
        std::int32_t n = 0;
        std::size_t part = 0;
        if (!readPositive(r, f, "n", n) || !readEnum(r, f, "sub-kind", kFactionWords, part)) {
            return false;
        }
        if (part == 0) {
            FactionInfoFields v;
            v.n = n;
            std::size_t role = 0;
            if (!readEnum(r, f, "role", kRoles, role) ||
                !readRange(r, f, "type", 1, 7, false, v.type) ||
                !readNonNegative(r, f, "power", v.power) ||
                !readNonNegative(r, f, "threshold", v.threshold) ||
                !readNonNegative(r, f, "members", v.members) || !expectEnd(r, f)) {
                return false;
            }
            v.role = role == 0 ? FactionRole::Against : FactionRole::Member;
            record.fields = v;
            return true;
        }
        std::string_view first;
        bool none = false;
        if (!readNoneOrFirst(r, f, "faction record", none, first)) {
            return false;
        }
        if (part == 3) {
            FactionTitleFields v;
            v.n = n;
            v.none = none;
            if (!none) {
                if (!intField(first, f, "tier", 1, 5, false, v.tier) || !readText(r, f, record)) {
                    return false;
                }
                hasText = true;
            }
            record.fields = v;
            return true;
        }
        FactionCharFields v;
        v.n = n;
        v.part =
            static_cast<FactionPart>(part);  // Leader, Claimant or Target: index of kFactionWords
        v.none = none;
        if (!none) {
            if (!intField(first, f, "id", 0, kIntMax, false, v.id) || !readText(r, f, record)) {
                return false;
            }
            hasText = true;
        }
        record.fields = v;
        return true;
    }
    case RecordType::End: {
        EndFields v;
        if (!readNonNegative(r, f, "count", v.count) || !expectEnd(r, f)) {
            return false;
        }
        record.fields = v;
        return true;
    }
    case RecordType::Unknown:
        break;
    }
    return fail(f, InvalidReason::Header, "type", "not a known type");
}

// Section 3: the raw text of the first three fields after the type, at most 64 bytes, control
// bytes shown as '?'. A cut that would split a UTF-8 sequence drops the partial sequence.
std::string leadingFieldsReport(std::string_view record, std::size_t afterType) {
    std::string_view rest =
        afterType < record.size() ? record.substr(afterType) : std::string_view();
    if (!rest.empty() && rest[0] == '|') {
        rest.remove_prefix(1);
    }
    std::size_t end = 0;
    int bars = 0;
    while (end < rest.size()) {
        if (rest[end] == '|' && ++bars == 3) {
            break;
        }
        ++end;
    }
    std::string_view head = rest.substr(0, end);
    if (head.size() > kMaxLeadingFieldsBytes) {
        head = trimIncompleteUtf8Tail(head.substr(0, kMaxLeadingFieldsBytes));
    }
    std::string out(head);
    for (char& c : out) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20 || byte == 0x7F) {
            c = '?';
        }
    }
    return out;
}

ParseResult makeInvalid(InvalidReason reason, std::string detail, std::optional<std::int32_t> snap,
                        std::string_view typeWord, std::string_view record, std::size_t afterType) {
    ParseResult result;
    result.outcome = ParseOutcome::Invalid;
    result.invalid.reason = reason;
    result.invalid.detail = std::move(detail);
    result.invalid.snap = snap;
    result.invalid.isBegin = snap.has_value() && typeWord == "BEGIN";
    if (result.invalid.isBegin) {
        result.invalid.leadingFields = leadingFieldsReport(record, afterType);
    }
    return result;
}

}  // namespace

std::string_view trimIncompleteUtf8Tail(std::string_view s) noexcept {
    const std::size_t n = s.size();
    for (std::size_t back = 1; back <= 3 && back <= n; ++back) {
        const unsigned byte = static_cast<unsigned char>(s[n - back]);
        if ((byte & 0xC0U) == 0x80U) {
            continue;  // a continuation byte: the sequence starts further back
        }
        std::size_t expected = 1;
        if (byte >= 0xC2U && byte <= 0xDFU) {
            expected = 2;
        } else if (byte >= 0xE0U && byte <= 0xEFU) {
            expected = 3;
        } else if (byte >= 0xF0U && byte <= 0xF4U) {
            expected = 4;
        }
        if (expected > back) {
            return {s.data(), n - back};
        }
        return s;
    }
    return s;
}

ParseResult parseLine(std::string_view line, bool overlong) {
    // Step 1: the first marker; at offset 0 (no engine prefix) it is refused.
    const std::size_t markerPos = line.find(kMarker);
    if (markerPos == std::string_view::npos || markerPos == 0) {
        return ParseResult{};
    }
    const std::string_view full = line.substr(markerPos);

    // Step 2: the header.
    const std::string_view afterMarker = full.substr(kMarker.size());
    const std::size_t snapBar = afterMarker.find('|');
    const std::string_view snapText =
        snapBar == std::string_view::npos ? afterMarker : afterMarker.substr(0, snapBar);
    std::optional<std::int32_t> snap;
    {
        std::int32_t value = 0;
        if (parseInt32(snapText, false, value) && value >= 0) {
            snap = value;
        }
    }
    if (snapBar == std::string_view::npos) {
        return makeInvalid(InvalidReason::Header, "header: no type", snap, {}, full, 0);
    }
    const std::size_t typeStart = kMarker.size() + snapBar + 1;
    const std::size_t typeBar = full.find('|', typeStart);
    const std::size_t typeEnd = typeBar == std::string_view::npos ? full.size() : typeBar;
    const std::string_view typeWord = full.substr(typeStart, typeEnd - typeStart);
    if (!snap.has_value()) {
        return makeInvalid(InvalidReason::Header, "header: snap is not an integer in range", snap,
                           typeWord, full, typeEnd);
    }
    const std::int32_t snapValue = *snap;
    if (!isTypeWord(typeWord)) {
        return makeInvalid(InvalidReason::Header, "header: type word does not match", snap,
                           typeWord, full, typeEnd);
    }

    // Step 3: snap 0 belongs to LOAD only.
    const bool isLoad = typeWord == "LOAD";
    if (snapValue == 0 && !isLoad) {
        return makeInvalid(InvalidReason::SnapRule, "snap 0 is valid only on LOAD", snap, typeWord,
                           full, typeEnd);
    }
    if (snapValue != 0 && isLoad) {
        return makeInvalid(InvalidReason::SnapRule, "LOAD needs snap 0", snap, typeWord, full,
                           typeEnd);
    }

    // Step 4: unknown types (and unknown sub-kinds) get a header-only check.
    const RecordType type = lookupType(typeWord);
    bool known = type != RecordType::Unknown;
    std::string_view subWord;
    if (type == RecordType::War || type == RecordType::Faction) {
        Reader r(full, typeEnd);
        Failure f;
        std::int32_t n = 0;
        if (!readPositive(r, f, "n", n)) {
            return makeInvalid(InvalidReason::SubKind, std::move(f.detail), snap, typeWord, full,
                               typeEnd);
        }
        if (!r.next(subWord)) {
            return makeInvalid(InvalidReason::SubKind, "sub-kind: missing", snap, typeWord, full,
                               typeEnd);
        }
        if (!isTypeWord(subWord)) {
            return makeInvalid(InvalidReason::SubKind, "sub-kind: does not match the word pattern",
                               snap, typeWord, full, typeEnd);
        }
        const std::span<const std::string_view> words =
            type == RecordType::War ? std::span<const std::string_view>(kWarWords)
                                    : std::span<const std::string_view>(kFactionWords);
        if (indexOfWord(words, subWord) == kNoSubKind) {
            known = false;
        }
    }

    ParseResult result;
    result.outcome = ParseOutcome::Record;
    Record& record = result.record;
    record.snap = snapValue;
    record.typeName.assign(typeWord);
    record.subKind.assign(subWord);
    if (!known) {
        record.type = RecordType::Unknown;
        record.length = full.size();
        return result;
    }

    // Step 5: the field grammar, on the record as cut by the oversize rule (section 1).
    const bool oversize = overlong || full.size() > kMaxRecordBytes;
    std::string_view work = full;
    if (oversize) {
        work = trimIncompleteUtf8Tail(full.substr(0, std::min(full.size(), kMaxRecordBytes)));
    }
    record.type = type;
    Reader reader(work, typeEnd);
    Failure failure;
    bool hasText = false;
    if (!parseBody(type, reader, failure, record, hasText)) {
        return makeInvalid(failure.reason, std::move(failure.detail), snap, typeWord, work,
                           typeEnd);
    }
    if (oversize) {
        if (!hasText) {
            return makeInvalid(InvalidReason::Oversize, "oversize record without a text field",
                               snap, typeWord, work, typeEnd);
        }
        record.textShortened = true;
    }
    record.length = work.size();
    return result;
}

}  // namespace curia::bridge
