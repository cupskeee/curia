// SPDX-License-Identifier: MIT
#pragma once

#include <string_view>

namespace curia::advisor {

// Bumped whenever the persona text changes in meaning (the app log records it, so a quality
// comparison between two runs says which text was used).
inline constexpr int kPersonaVersion = 2;

// The persona: the first system block of every request. It is the same bytes for the whole life of
// the process, so the provider can cache it. The text speaks as "the advisor"; it names no product
// or model. The owner reviews it by eye (the tests check required phrases and length, not a hash).
std::string_view persona();

}  // namespace curia::advisor
