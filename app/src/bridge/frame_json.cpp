// SPDX-License-Identifier: MIT
#include "bridge/frame_json.h"

#include <utility>

namespace curia::bridge {

namespace {

using Json = nlohmann::json;

Json characterJson(const std::optional<CharacterRef>& ref) {
    if (!ref.has_value()) {
        return nullptr;
    }
    return Json{{"id", ref->id}, {"name", ref->name}};
}

Json titleJson(const std::optional<TitleRef>& ref) {
    if (!ref.has_value()) {
        return nullptr;
    }
    return Json{{"tier", ref->tier}, {"title", ref->title}};
}

const char* roleName(FactionRole role) noexcept {
    return role == FactionRole::Against ? "against" : "member";
}

const char* kindName(ClaimKind kind) noexcept {
    return kind == ClaimKind::Strong ? "strong" : "weak";
}

}  // namespace

const char* toString(DamageReason reason) noexcept {
    switch (reason) {
    case DamageReason::Incomplete:
        return "incomplete";
    case DamageReason::InvalidRecord:
        return "invalid_record";
    case DamageReason::SnapMismatch:
        return "snap_mismatch";
    case DamageReason::CountMismatch:
        return "count_mismatch";
    case DamageReason::Order:
        return "order";
    case DamageReason::TooLarge:
        return "too_large";
    case DamageReason::TruncatedLog:
        return "truncated_log";
    }
    return "unknown";
}

const char* toString(InvalidReason reason) noexcept {
    switch (reason) {
    case InvalidReason::Header:
        return "header";
    case InvalidReason::SnapRule:
        return "snap_rule";
    case InvalidReason::SubKind:
        return "sub_kind";
    case InvalidReason::FieldCount:
        return "field_count";
    case InvalidReason::FieldValue:
        return "field_value";
    case InvalidReason::NoneForm:
        return "none_form";
    case InvalidReason::Oversize:
        return "oversize";
    }
    return "unknown";
}

Json toJson(const Frame& frame) {
    Json out = Json::object();
    out["snap"] = frame.snap;
    out["mode"] = frame.mode;
    out["mod_version"] = frame.modVersion;
    out["date"] =
        Json{{"year", frame.date.year}, {"month", frame.date.month}, {"day", frame.date.day}};
    out["ruler"] = Json{{"id", frame.rulerId}, {"name", frame.rulerName}};
    out["text_shortened"] = frame.textShortened;
    out["record_count"] = frame.recordCount;
    if (frame.fin.has_value()) {
        out["fin"] = Json{{"gold", frame.fin->gold}, {"income_tenths", frame.fin->incomeTenths}};
    }
    if (frame.heir.has_value()) {
        const Heir& heir = *frame.heir;
        if (heir.none) {
            out["heir"] = Json{{"none", true}};
        } else {
            out["heir"] = Json{{"none", false},           {"id", heir.id},
                               {"opinion", heir.opinion}, {"has_liege", heir.hasLiege},
                               {"landed", heir.landed},   {"name", heir.name}};
        }
    }
    if (frame.vassals.has_value()) {
        Json listed = Json::array();
        for (const Vassal& v : frame.vassals->listed) {
            listed.push_back(Json{{"rank", v.rank},
                                  {"id", v.id},
                                  {"gold", v.gold},
                                  {"opinion", v.opinion},
                                  {"strength", v.strength},
                                  {"name", v.name}});
        }
        out["vassals"] = Json{{"total", frame.vassals->total}, {"listed", std::move(listed)}};
    }
    if (frame.wars.has_value()) {
        Json wars = Json::array();
        for (const War& w : frame.wars->wars) {
            wars.push_back(
                Json{{"name", w.name},
                     {"attacker", Json{{"id", w.attacker.id}, {"name", w.attacker.name}}},
                     {"defender", Json{{"id", w.defender.id}, {"name", w.defender.name}}}});
        }
        out["wars"] = Json{{"total", frame.wars->total}, {"wars", std::move(wars)}};
    }
    if (frame.claims.has_value()) {
        Json listed = Json::array();
        for (const Claim& c : frame.claims->listed) {
            listed.push_back(
                Json{{"kind", kindName(c.kind)}, {"tier", c.tier}, {"title", c.title}});
        }
        out["claims"] = Json{{"explicit", frame.claims->explicitCount},
                             {"implicit", frame.claims->implicitCount},
                             {"listed", std::move(listed)}};
    }
    if (frame.factions.has_value()) {
        Json listed = Json::array();
        for (const Faction& f : frame.factions->listed) {
            listed.push_back(Json{{"role", roleName(f.role)},
                                  {"type", f.type},
                                  {"power", f.power},
                                  {"threshold", f.threshold},
                                  {"members", f.members},
                                  {"leader", characterJson(f.leader)},
                                  {"claimant", characterJson(f.claimant)},
                                  {"title", titleJson(f.title)},
                                  {"target", characterJson(f.target)}});
        }
        out["factions"] = Json{{"against", frame.factions->against},
                               {"member", frame.factions->member},
                               {"listed", std::move(listed)}};
    }
    return out;
}

Json toJson(const Event& event) {
    if (const auto* complete = std::get_if<FrameComplete>(&event)) {
        return Json{{"event", "frame_complete"}, {"frame", toJson(complete->frame)}};
    }
    if (const auto* damaged = std::get_if<FrameDamaged>(&event)) {
        return Json{{"event", "frame_damaged"},
                    {"snap", damaged->snap},
                    {"reason", toString(damaged->reason)},
                    {"detail", damaged->detail}};
    }
    if (const auto* unsupported = std::get_if<UnsupportedFormat>(&event)) {
        return Json{{"event", "unsupported_format"},
                    {"snap", unsupported->snap},
                    {"leading_fields", unsupported->leadingFields}};
    }
    const auto* diagnostic = std::get_if<Diagnostic>(&event);
    Json snap = nullptr;
    if (diagnostic != nullptr && diagnostic->snap.has_value()) {
        snap = *diagnostic->snap;
    }
    return Json{{"event", "diagnostic"},
                {"snap", std::move(snap)},
                {"reason", diagnostic != nullptr ? toString(diagnostic->reason) : "header"},
                {"detail", diagnostic != nullptr ? diagnostic->detail : std::string()}};
}

Json toJson(const std::vector<Event>& events) {
    Json out = Json::array();
    for (const Event& event : events) {
        out.push_back(toJson(event));
    }
    return out;
}

std::string dumpJson(const Json& json) {
    std::string text = json.dump(2, ' ', false, Json::error_handler_t::replace);
    text.push_back('\n');
    return text;
}

std::string dumpJsonLine(const Json& json) {
    return json.dump(-1, ' ', false, Json::error_handler_t::replace);
}

}  // namespace curia::bridge
