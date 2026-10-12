// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "providers/sse_parser.h"

// Two small helpers of the Anthropic layer (request builder, error mapping, stream decoder): JSON
// from the network parsed without recursion hazards, and server text made safe to show.
namespace curia::providers {

// The deepest nesting of arrays and objects accepted from the network. A real message is about
// five levels deep; anything beyond this is hostile or broken.
inline constexpr int kMaxJsonDepth = 32;

// Parses `text` as one JSON value, or returns nullopt (never throws). Refused without parsing:
// text longer than `maxBytes`, and text nested deeper than `maxDepth`; the depth is counted by a
// scan of the brackets outside strings, so the parser (and the destructor of the result) never
// meets a deep structure. Invalid UTF-8 inside a string makes the parse fail, so every string in
// the result is valid UTF-8.
std::optional<nlohmann::json> parseBoundedJson(std::string_view text,
                                               std::size_t maxBytes = SseParser::kMaxEventBytes,
                                               int maxDepth = kMaxJsonDepth);

// A short, printable, valid UTF-8 rendering of untrusted text for messages and logs: at most
// `maxBytes` bytes, cut at a character boundary (never inside a UTF-8 sequence); control
// characters (including C1) and line breaks become spaces; a byte that is not part of valid UTF-8
// becomes '?'; leading and trailing spaces are removed. Everything else passes through unchanged.
// It does not redact: the caller does that.
std::string printableExcerpt(std::string_view text, std::size_t maxBytes);

}  // namespace curia::providers
