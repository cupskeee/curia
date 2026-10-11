// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace curia::bridge {

// Step 1 of the hygiene of docs/wire-format.md section 2: repairs invalid UTF-8. Every maximal
// invalid subsequence (the WHATWG "maximal subpart" rule) becomes one U+FFFD; the output is valid
// UTF-8 (overlong forms, surrogates and code points above U+10FFFF count as invalid).
std::string repairUtf8(std::string_view raw);

// All five hygiene steps, in the order of the spec: UTF-8 repair, deletion of the byte pair
// 0x15 0x21, link-tag removal (0x15 followed by a tag word of "L", "L;", "ONCLICK:..." or
// "TOOLTIP:..."), deletion of the remaining control and bidirectional-format code points, and
// collapse and trim of U+0020. "Space" in the tag-word rule is U+0020 only, the same character the
// last step collapses. Applied by the frame assembler to every text field it copies into a frame.
std::string cleanText(std::string_view raw);

}  // namespace curia::bridge
