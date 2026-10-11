// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "bridge/record.h"

namespace curia::bridge {

// One complete realm snapshot: a frame of docs/wire-format.md section 3 that passed every check.
// All text is hygiene-cleaned (text_hygiene.h). A section that is std::nullopt was not exported
// ("absent means not exported, never empty"); a section that is present but empty has no items.

struct Date {
    std::int32_t year = 0;
    std::int32_t month = 0;
    std::int32_t day = 0;
};

struct Finances {
    std::int32_t gold = 0;
    // Net monthly balance (income minus expenses, the top bar figure) in tenths of a gold piece
    // (78 means +7.8); may be negative.
    std::int32_t incomeTenths = 0;
};

// The `none` form of HEIR has none == true and nothing else set.
struct Heir {
    bool none = false;
    std::int32_t id = 0;
    std::int32_t opinion = 0;
    bool hasLiege = false;
    bool landed = false;
    std::string name;
};

struct Vassal {
    std::int32_t rank = 0;
    std::int32_t id = 0;
    std::int32_t gold = 0;
    std::int32_t opinion = 0;
    std::int32_t strength = 0;
    std::string name;
};

struct VassalsSection {
    std::int32_t total = 0;
    std::vector<Vassal> listed;
};

struct WarSide {
    std::int32_t id = 0;
    std::string name;
};

struct War {
    std::string name;
    WarSide attacker;
    WarSide defender;
};

struct WarsSection {
    std::int32_t total = 0;
    std::vector<War> wars;
};

struct Claim {
    ClaimKind kind = ClaimKind::Weak;
    std::int32_t tier = 0;
    std::string title;
};

struct ClaimsSection {
    std::int32_t explicitCount = 0;
    std::int32_t implicitCount = 0;
    std::vector<Claim> listed;
};

struct CharacterRef {
    std::int32_t id = 0;
    std::string name;
};

struct TitleRef {
    std::int32_t tier = 0;
    std::string title;
};

// A faction group. The optional members are std::nullopt for the `none` form. The claimant and the
// title exist for type 3 (claimant faction) only: the mod writes `none` for other types, and the
// assembler drops a value in a group of another type (valid in the record, ignored here, so that a
// consumer cannot mistake a special character of another faction type for a claimant).
struct Faction {
    FactionRole role = FactionRole::Against;
    std::int32_t type = 0;
    std::int32_t power = 0;
    std::int32_t threshold = 0;
    std::int32_t members = 0;
    std::optional<CharacterRef> leader;
    std::optional<CharacterRef> claimant;
    std::optional<TitleRef> title;
    std::optional<CharacterRef> target;
};

struct FactionsSection {
    std::int32_t against = 0;
    bool member = false;
    std::vector<Faction> listed;
};

struct Frame {
    std::int32_t snap = 0;
    // 0 = Advisor click, 1 = probe click.
    std::int32_t mode = 0;
    std::string modVersion;
    Date date;
    std::int32_t rulerId = 0;
    std::string rulerName;
    std::optional<Finances> fin;
    std::optional<Heir> heir;
    std::optional<VassalsSection> vassals;
    std::optional<WarsSection> wars;
    std::optional<ClaimsSection> claims;
    std::optional<FactionsSection> factions;
    // A text field of the frame was shortened by the oversize rule (a note, not damage).
    bool textShortened = false;
    // Frame records counted by END, BEGIN and END included.
    std::int32_t recordCount = 0;
};

}  // namespace curia::bridge
