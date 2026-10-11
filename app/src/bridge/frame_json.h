// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "bridge/events.h"
#include "bridge/frame.h"

// JSON form of frames and events, used by the golden tests (tests/fixtures/curia1/*.events.json),
// later by a command-line tool and the end-to-end test. Keys are stable snake_case; objects are
// printed with sorted keys, so the output is fully deterministic. The same numbers and texts as
// the C++ types, nothing derived.
//
// Event (an array of these is the golden file):
//   {"event": "frame_complete", "frame": <frame>}
//   {"event": "frame_damaged", "snap": int, "reason": <damage reason>, "detail": string}
//   {"event": "unsupported_format", "snap": int, "leading_fields": string}
//   {"event": "diagnostic", "snap": int or null, "reason": <invalid reason>, "detail": string}
// Damage reason: incomplete, invalid_record, snap_mismatch, count_mismatch, order, too_large,
//   truncated_log.
// Invalid reason: header, snap_rule, sub_kind, field_count, field_value, none_form, oversize.
//
// Frame: {"snap", "mode", "mod_version", "date": {"year", "month", "day"},
//   "ruler": {"id", "name"}, "text_shortened": bool, "record_count": int, and for each section that
//   was exported (a key is absent when its section was not):
//   "fin": {"gold", "income_tenths"},
//   "heir": {"none": true} or {"none": false, "id", "opinion", "has_liege", "landed", "name"},
//   "vassals": {"total", "listed": [{"rank", "id", "gold", "opinion", "strength", "name"}]},
//   "wars": {"total", "wars": [{"name", "attacker": {"id", "name"}, "defender": {"id", "name"}}]},
//   "claims": {"explicit", "implicit", "listed": [{"kind": "strong"|"weak", "tier", "title"}]},
//   "factions": {"against", "member": bool, "listed": [{"role": "against"|"member", "type",
//     "power", "threshold", "members", "leader": null or {"id", "name"}, "claimant": same,
//     "title": null or {"tier", "title"}, "target": same}]}}
namespace curia::bridge {

const char* toString(DamageReason reason) noexcept;
const char* toString(InvalidReason reason) noexcept;

nlohmann::json toJson(const Frame& frame);
nlohmann::json toJson(const Event& event);
nlohmann::json toJson(const std::vector<Event>& events);

// Pretty-printed (two spaces), invalid UTF-8 replaced instead of throwing, ending in a newline.
std::string dumpJson(const nlohmann::json& json);

// One line: compact, invalid UTF-8 replaced, no trailing newline (the JSON Lines form of the
// headless watch mode).
std::string dumpJsonLine(const nlohmann::json& json);

}  // namespace curia::bridge
