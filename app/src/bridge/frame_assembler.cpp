// SPDX-License-Identifier: MIT
#include "bridge/frame_assembler.h"

#include <algorithm>
#include <utility>

#include "bridge/text_hygiene.h"

namespace curia::bridge {

namespace {

// What the structure check of section 3 found. Count violations and order violations are kept
// apart because END reports the first that applies of count mismatch, then order.
struct Verdict {
    bool count = false;
    bool order = false;
    std::string countDetail;
    std::string orderDetail;
};

void noteCount(Verdict& v, const char* why) {
    if (!v.count) {
        v.count = true;
        v.countDetail = why;
    }
}

void noteOrder(Verdict& v, const char* why) {
    if (!v.order) {
        v.order = true;
        v.orderDetail = why;
    }
}

constexpr int kSectionFin = 0;
constexpr int kSectionHeir = 1;
constexpr int kSectionVassals = 2;
constexpr int kSectionWars = 3;
constexpr int kSectionClaims = 4;
constexpr int kSectionFactions = 5;

constexpr std::size_t kWarGroupSize = 3;
constexpr std::size_t kFactionGroupSize = 5;

// Checks the section rules and counts (section 3) over the known-type records of a frame, BEGIN
// first, and fills `frame` as it goes. The checks do not stop at the first violation: whatever is
// found decides the reason, and the frame is thrown away when anything was found. A repeated
// section header replaces the earlier section so that one fault does not show up as another.
Verdict buildFrame(const std::vector<Record>& records, Frame& frame) {
    Verdict verdict;
    int lastSection = -1;
    int currentSection = -1;
    const auto startSection = [&](int section) {
        if (section <= lastSection) {
            noteOrder(verdict, "section repeated or out of order");
        }
        lastSection = std::max(lastSection, section);
        currentSection = section;
    };

    std::int32_t vassalsListed = 0;
    std::int32_t claimsListed = 0;
    std::int32_t factionsListed = 0;
    std::size_t warPos = 0;
    std::int32_t warN = 0;
    std::size_t factionPos = 0;
    std::int32_t factionN = 0;

    for (std::size_t i = 0; i < records.size(); ++i) {
        const Record& rec = records[i];
        if (i == 0) {
            const auto* begin = std::get_if<BeginFields>(&rec.fields);
            if (begin == nullptr) {
                noteOrder(verdict, "the first record is not BEGIN");
                continue;
            }
            frame.mode = begin->mode;
            frame.modVersion = begin->modVersion;
            frame.date = Date{begin->year, begin->month, begin->day};
            frame.rulerId = begin->rulerId;
            frame.rulerName = cleanText(rec.text);
            continue;
        }
        switch (rec.type) {
        case RecordType::Fin: {
            startSection(kSectionFin);
            if (const auto* v = std::get_if<FinFields>(&rec.fields)) {
                frame.fin = Finances{v->gold, v->income};
            }
            break;
        }
        case RecordType::Heir: {
            startSection(kSectionHeir);
            if (const auto* v = std::get_if<HeirFields>(&rec.fields)) {
                Heir heir;
                heir.none = v->none;
                if (!v->none) {
                    heir.id = v->id;
                    heir.opinion = v->opinion;
                    heir.hasLiege = v->hasLiege;
                    heir.landed = v->landed;
                    heir.name = cleanText(rec.text);
                }
                frame.heir = std::move(heir);
            }
            break;
        }
        case RecordType::Vassals: {
            startSection(kSectionVassals);
            if (const auto* v = std::get_if<VassalsFields>(&rec.fields)) {
                VassalsSection section;
                section.total = v->total;
                frame.vassals = std::move(section);
                vassalsListed = v->listed;
            }
            break;
        }
        case RecordType::Vassal: {
            const auto* v = std::get_if<VassalFields>(&rec.fields);
            if (currentSection != kSectionVassals || !frame.vassals.has_value() || v == nullptr) {
                noteOrder(verdict, "VASSAL outside its section");
                break;
            }
            auto& items = frame.vassals->listed;
            if (static_cast<std::size_t>(v->rank) != items.size() + 1) {
                noteOrder(verdict, "VASSAL rank does not run 1..listed");
            }
            items.push_back(
                Vassal{v->rank, v->id, v->gold, v->opinion, v->strength, cleanText(rec.text)});
            break;
        }
        case RecordType::Wars: {
            startSection(kSectionWars);
            if (const auto* v = std::get_if<WarsFields>(&rec.fields)) {
                WarsSection section;
                section.total = v->total;
                frame.wars = std::move(section);
                warPos = 0;
                warN = 0;
            }
            break;
        }
        case RecordType::War: {
            const auto* v = std::get_if<WarFields>(&rec.fields);
            if (currentSection != kSectionWars || !frame.wars.has_value() || v == nullptr) {
                noteOrder(verdict, "WAR outside its section");
                break;
            }
            auto& wars = frame.wars->wars;
            const auto part = static_cast<std::size_t>(v->part);
            if (part != warPos) {
                noteOrder(verdict, "WAR records not NAME, ATTACKER, DEFENDER in order");
            }
            if (v->part == WarPart::Name) {
                wars.emplace_back();
                wars.back().name = cleanText(rec.text);
                if (static_cast<std::size_t>(v->n) != wars.size()) {
                    noteOrder(verdict, "WAR n does not run 1..total");
                }
                warN = v->n;
            } else if (wars.empty()) {
                noteOrder(verdict, "WAR side before its NAME");
            } else {
                if (v->n != warN) {
                    noteOrder(verdict, "WAR records of one group differ in n");
                }
                WarSide& side =
                    v->part == WarPart::Attacker ? wars.back().attacker : wars.back().defender;
                side.id = v->id;
                side.name = cleanText(rec.text);
            }
            warPos = (part + 1) % kWarGroupSize;
            break;
        }
        case RecordType::Claims: {
            startSection(kSectionClaims);
            if (const auto* v = std::get_if<ClaimsFields>(&rec.fields)) {
                ClaimsSection section;
                section.explicitCount = v->explicitCount;
                section.implicitCount = v->implicitCount;
                frame.claims = std::move(section);
                claimsListed = v->listed;
            }
            break;
        }
        case RecordType::Claim: {
            const auto* v = std::get_if<ClaimFields>(&rec.fields);
            if (currentSection != kSectionClaims || !frame.claims.has_value() || v == nullptr) {
                noteOrder(verdict, "CLAIM outside its section");
                break;
            }
            auto& items = frame.claims->listed;
            if (static_cast<std::size_t>(v->n) != items.size() + 1) {
                noteOrder(verdict, "CLAIM n does not run 1..listed");
            }
            items.push_back(Claim{v->kind, v->tier, cleanText(rec.text)});
            break;
        }
        case RecordType::Factions: {
            startSection(kSectionFactions);
            if (const auto* v = std::get_if<FactionsFields>(&rec.fields)) {
                FactionsSection section;
                section.against = v->against;
                section.member = v->member;
                frame.factions = std::move(section);
                factionsListed = v->listed;
                factionPos = 0;
                factionN = 0;
            }
            break;
        }
        case RecordType::Faction: {
            if (currentSection != kSectionFactions || !frame.factions.has_value()) {
                noteOrder(verdict, "FACTION outside its section");
                break;
            }
            auto& groups = frame.factions->listed;
            std::size_t part = 0;
            std::int32_t n = 0;
            const auto* info = std::get_if<FactionInfoFields>(&rec.fields);
            if (info != nullptr) {
                n = info->n;
            } else if (const auto* c = std::get_if<FactionCharFields>(&rec.fields)) {
                part = static_cast<std::size_t>(c->part);
                n = c->n;
            } else if (const auto* t = std::get_if<FactionTitleFields>(&rec.fields)) {
                part = static_cast<std::size_t>(FactionPart::Title);
                n = t->n;
            } else {
                noteOrder(verdict, "FACTION record without fields");
                break;
            }
            if (part != factionPos) {
                noteOrder(verdict, "FACTION records not INFO, LEADER, CLAIMANT, TITLE, TARGET");
            }
            if (info != nullptr) {
                Faction faction;
                faction.role = info->role;
                faction.type = info->type;
                faction.power = info->power;
                faction.threshold = info->threshold;
                faction.members = info->members;
                groups.push_back(std::move(faction));
                if (static_cast<std::size_t>(n) != groups.size()) {
                    noteOrder(verdict, "FACTION n does not run 1..listed");
                }
                factionN = n;
            } else if (groups.empty()) {
                noteOrder(verdict, "FACTION record before its INFO");
            } else {
                if (n != factionN) {
                    noteOrder(verdict, "FACTION records of one group differ in n");
                }
                Faction& faction = groups.back();
                if (const auto* c = std::get_if<FactionCharFields>(&rec.fields)) {
                    std::optional<CharacterRef> ref;
                    if (!c->none) {
                        ref = CharacterRef{c->id, cleanText(rec.text)};
                    }
                    if (c->part == FactionPart::Leader) {
                        faction.leader = std::move(ref);
                    } else if (c->part == FactionPart::Claimant) {
                        faction.claimant = std::move(ref);
                    } else {
                        faction.target = std::move(ref);
                    }
                } else if (const auto* t = std::get_if<FactionTitleFields>(&rec.fields)) {
                    if (!t->none) {
                        faction.title = TitleRef{t->tier, cleanText(rec.text)};
                    } else {
                        faction.title.reset();
                    }
                }
            }
            factionPos = (part + 1) % kFactionGroupSize;
            break;
        }
        case RecordType::Load:
        case RecordType::Begin:
        case RecordType::End:
        case RecordType::Unknown:
            // Not stored (LOAD has snap 0, a second BEGIN opens a new frame, END is handled by the
            // caller, unknown types are only counted).
            noteOrder(verdict, "unexpected record kept in a frame");
            break;
        }
    }

    // Checked counts, once every section is complete.
    if (frame.vassals.has_value()) {
        const auto& v = *frame.vassals;
        if (static_cast<std::int64_t>(v.listed.size()) != vassalsListed) {
            noteCount(verdict, "VASSALS listed differs from the VASSAL records");
        }
        if (vassalsListed > v.total) {
            noteCount(verdict, "VASSALS listed exceeds total");
        }
    }
    if (frame.wars.has_value()) {
        if (static_cast<std::int64_t>(frame.wars->wars.size()) != frame.wars->total) {
            noteCount(verdict, "WARS total differs from the WAR groups");
        }
        if (warPos != 0) {
            noteOrder(verdict, "WAR group incomplete");
        }
    }
    if (frame.claims.has_value()) {
        const auto& c = *frame.claims;
        if (static_cast<std::int64_t>(c.listed.size()) != claimsListed) {
            noteCount(verdict, "CLAIMS listed differs from the CLAIM records");
        }
        if (claimsListed > c.explicitCount) {
            noteCount(verdict, "CLAIMS listed exceeds explicit");
        }
    }
    if (frame.factions.has_value()) {
        const auto& f = *frame.factions;
        if (static_cast<std::int64_t>(f.listed.size()) != factionsListed) {
            noteCount(verdict, "FACTIONS listed differs from the FACTION groups");
        }
        const std::int64_t memberGroups =
            std::count_if(f.listed.begin(), f.listed.end(),
                          [](const Faction& g) { return g.role == FactionRole::Member; });
        const std::int64_t againstGroups =
            static_cast<std::int64_t>(f.listed.size()) - memberGroups;
        if (memberGroups != (f.member ? 1 : 0)) {
            noteCount(verdict, "FACTIONS member differs from the groups with role member");
        }
        if (againstGroups > f.against) {
            noteCount(verdict, "FACTIONS groups with role against exceed against");
        }
        if (factionPos != 0) {
            noteOrder(verdict, "FACTION group incomplete");
        }
    }
    return verdict;
}

}  // namespace

std::vector<Event> FrameAssembler::consume(const ParseResult& result) {
    std::vector<Event> events;
    switch (result.outcome) {
    case ParseOutcome::NotCuria:
        break;
    case ParseOutcome::Invalid:
        handleInvalid(result.invalid, events);
        break;
    case ParseOutcome::Record:
        handleRecord(result.record, events);
        break;
    }
    return events;
}

std::vector<Event> FrameAssembler::truncateLog() {
    std::vector<Event> events;
    if (state_ == State::Open) {
        damage(DamageReason::TruncatedLog, "the log was truncated inside a frame", events);
    }
    reset();
    return events;
}

void FrameAssembler::handleInvalid(const InvalidRecord& invalid, std::vector<Event>& events) {
    const std::int32_t snap = invalid.snap.value_or(-1);
    if (snap == 0) {
        // Snap 0 never touches a frame; only the log hears of it.
        events.emplace_back(Diagnostic{invalid.snap, invalid.reason, invalid.detail});
        return;
    }
    if (invalid.isBegin && snap >= 1) {
        if (state_ == State::Open) {
            damage(DamageReason::Incomplete, "an unsupported BEGIN replaced the open frame",
                   events);
        }
        events.emplace_back(UnsupportedFormat{snap, invalid.leadingFields});
        reset();
        return;
    }
    switch (state_) {
    case State::Idle:
        events.emplace_back(Diagnostic{invalid.snap, invalid.reason, invalid.detail});
        break;
    case State::Open:
        damage(DamageReason::InvalidRecord, invalid.detail, events);
        break;
    case State::Damaged:
        break;
    }
}

void FrameAssembler::handleRecord(const Record& record, std::vector<Event>& events) {
    if (record.snap == 0) {
        return;  // a valid LOAD: informational, never a frame record
    }
    if (record.type == RecordType::Begin) {
        if (state_ == State::Open) {
            damage(DamageReason::Incomplete, "a new BEGIN arrived before END", events);
        }
        openFrame(record);
        return;
    }
    if (state_ == State::Damaged) {
        if (record.type == RecordType::End) {
            reset();  // the damaged frame is over; it was reported when it was damaged
        }
        return;
    }
    if (state_ != State::Open) {
        return;  // Idle: the watcher may have attached mid-frame
    }
    ++count_;
    bytes_ += record.length;
    if (count_ > kMaxFrameRecords || bytes_ > kMaxFrameBytes) {
        damage(DamageReason::TooLarge, "the frame exceeds 512 records or 128 KiB", events);
        return;
    }
    if (record.snap != snap_) {
        const std::string detail = "record with snap " + std::to_string(record.snap) +
                                   " inside the frame with snap " + std::to_string(snap_);
        if (record.type == RecordType::End) {
            events.emplace_back(FrameDamaged{snap_, DamageReason::SnapMismatch, detail});
            reset();
        } else {
            damage(DamageReason::SnapMismatch, detail, events);
        }
        return;
    }
    if (record.type == RecordType::End) {
        finish(record, events);
        return;
    }
    if (record.textShortened) {
        shortened_ = true;
    }
    if (record.type != RecordType::Unknown) {
        records_.push_back(record);
    }
}

void FrameAssembler::openFrame(const Record& begin) {
    reset();
    state_ = State::Open;
    snap_ = begin.snap;
    count_ = 1;
    bytes_ = begin.length;
    shortened_ = begin.textShortened;
    records_.push_back(begin);
}

void FrameAssembler::damage(DamageReason reason, std::string detail, std::vector<Event>& events) {
    events.emplace_back(FrameDamaged{snap_, reason, std::move(detail)});
    state_ = State::Damaged;
    records_.clear();
}

void FrameAssembler::finish(const Record& end, std::vector<Event>& events) {
    const auto* endFields = std::get_if<EndFields>(&end.fields);
    const std::int64_t claimed = endFields != nullptr ? endFields->count : -1;
    Frame frame;
    const Verdict verdict = buildFrame(records_, frame);
    if (claimed != static_cast<std::int64_t>(count_)) {
        events.emplace_back(FrameDamaged{snap_, DamageReason::CountMismatch,
                                         "END count " + std::to_string(claimed) + ", counted " +
                                             std::to_string(count_)});
    } else if (verdict.count) {
        events.emplace_back(FrameDamaged{snap_, DamageReason::CountMismatch, verdict.countDetail});
    } else if (verdict.order) {
        events.emplace_back(FrameDamaged{snap_, DamageReason::Order, verdict.orderDetail});
    } else {
        frame.snap = snap_;
        frame.textShortened = shortened_;
        frame.recordCount = static_cast<std::int32_t>(count_);
        events.emplace_back(FrameComplete{std::move(frame)});
    }
    reset();
}

void FrameAssembler::reset() {
    state_ = State::Idle;
    snap_ = 0;
    count_ = 0;
    bytes_ = 0;
    shortened_ = false;
    records_.clear();
}

}  // namespace curia::bridge
