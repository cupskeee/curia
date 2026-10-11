// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>

#include "bridge/frame.h"
#include "bridge/record.h"

namespace curia::bridge {

// Why a frame was discarded (docs/wire-format.md section 3).
enum class DamageReason {
    Incomplete,
    InvalidRecord,
    SnapMismatch,
    CountMismatch,
    Order,
    TooLarge,
    TruncatedLog,
};

struct FrameComplete {
    Frame frame;
};

// Exactly one per damaged frame.
struct FrameDamaged {
    std::int32_t snap = 0;
    DamageReason reason = DamageReason::Incomplete;
    // Short human-readable cause for the app's log; not a stable format.
    std::string detail;
};

// A BEGIN that is not valid CURIA1 (for example the layout of an older mod): the app tells the
// player that mod and app do not match.
struct UnsupportedFormat {
    std::int32_t snap = 0;
    // The first three fields after the type, at most 64 bytes, control bytes shown as '?'.
    std::string leadingFields;
};

// An invalid record that damages no frame and is only reported to the app's log.
struct Diagnostic {
    std::optional<std::int32_t> snap;
    InvalidReason reason = InvalidReason::Header;
    std::string detail;
};

using Event = std::variant<FrameComplete, FrameDamaged, UnsupportedFormat, Diagnostic>;

}  // namespace curia::bridge
