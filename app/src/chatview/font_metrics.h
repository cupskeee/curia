// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>

// The vertical metrics of a TrueType or OpenType font file, from its 'head' and 'hhea' tables.
// ImGui (its stb_truetype loader) sizes a face so that the 'hhea' ascender-to-descender span fills
// the size it is given, not the em. A face with room for stacked diacritics (the bundled one spans
// 1.46 em) then draws smaller letters than the size suggests, so the chat window gives ImGui the
// size that makes the em what it wants (ui/theme.h). Pure: reads only the bytes it is given.
namespace curia::chatview {

struct FontVerticalMetrics {
    int unitsPerEm = 0;
    // 'hhea' ascender and descender, in font units (the descender is 0 or negative).
    int ascender = 0;
    int descender = 0;

    // ImGui's font size per em: (ascender - descender) / unitsPerEm.
    [[nodiscard]] float heightPerEm() const;
};

// Empty when `data` is not a single font file (a font collection is refused), a table is missing
// or reaches past the end, or the values are impossible (unitsPerEm outside 16 to 16384, the
// ascender not above the descender).
std::optional<FontVerticalMetrics> readVerticalMetrics(const unsigned char* data, std::size_t size);

}  // namespace curia::chatview
