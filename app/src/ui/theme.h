// SPDX-License-Identifier: MIT
#pragma once

#include <imgui.h>

#include <cstdint>

// The look of the chat window (FR-OVL-3): parchment, dark-brown ink, a deep red accent, the bundled
// book face, rounded but restrained frames. One place for every colour and size, so the contrast
// figures in docs/architecture.md stay true.
namespace curia::ui {

// The size of the letters: the em of the text, in logical pixels before the display's content
// scale. ImGui is given the font size that makes the em this big (FontSet::heightPerEm).
inline constexpr float kTextEm = 14.0F;

struct FontSet {
    ImFont* regular = nullptr;
    ImFont* bold = nullptr;
    // Both bundled faces were loaded. False = ImGui's own font stands in for both (the smoke test
    // fails then).
    bool bundled = false;
    // ImGui's font size per em of the regular face, from the font's own metrics
    // (chatview/font_metrics.h): ImGui fills the size it is given with the face's ascender-to-
    // descender span, which is 1.46 em for the bundled face. 1 when the metrics could not be read
    // (the smoke test fails then for the bundled face) and for ImGui's own font.
    float heightPerEm = 1.0F;
    bool metricsRead = false;

    // The font size ImGui gets for the text (before the content scale).
    [[nodiscard]] float baseSize() const { return kTextEm * heightPerEm; }
};

// Adds the two bundled faces to the font atlas (once, after ImGui::CreateContext()). ImGui does not
// own or free the bytes: they are part of the executable.
FontSet loadFonts();

// Sets ImGui's colours and sizes, the text at fonts.baseSize(). `uiScale` is the display's
// content scale only: the renderer applies the pixel density through the framebuffer scale.
void applyTheme(float uiScale, const FontSet& fonts);

// An sRGB colour.
struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;

    ImU32 u32(std::uint8_t alpha = 255) const { return IM_COL32(r, g, b, alpha); }
    ImVec4 vec4(float alpha = 1.0F) const {
        return {static_cast<float>(r) / 255.0F, static_cast<float>(g) / 255.0F,
                static_cast<float>(b) / 255.0F, alpha};
    }
};

// The palette. Contrast of the text colours (WCAG 2 ratios, computed from these values):
//   ink on parchment 11.2, on an answer 12.6, on a question 9.7, on the input 13.4, on a notice
//   10.0, on an information 10.5; muted ink on parchment 6.2, on an answer 7.0, on the input 7.4,
//   on a question 5.3, on an information 5.8; the error colour on an answer 8.4; the accent
//   (labels, the bar of a warning) on parchment 8.5, on a notice 7.5; button text on the accent
//   9.5 (hovered 7.5, pressed 12.1); a disabled button's text 6.5; muted ink on a notice 5.5.
//   Not text (WCAG 1.4.11 asks for 3): the frame of the input box 3.6 against the parchment and
//   4.3 against the input.
namespace palette {
inline constexpr Rgb kParchment{0xF2, 0xE8, 0xCF};  // the window
inline constexpr Rgb kAnswer{0xFB, 0xF5, 0xE6};     // an answer's card
inline constexpr Rgb kQuestion{0xE8, 0xD7, 0xB0};   // a question's card
inline constexpr Rgb kInput{0xFF, 0xFC, 0xF3};      // the input box
inline constexpr Rgb kNotice{0xF0, 0xD9, 0xB5};     // a warning notice
inline constexpr Rgb kInfo{0xE9, 0xE1, 0xC9};       // an informational notice
inline constexpr Rgb kInk{0x3B, 0x2A, 0x1A};        // body text
inline constexpr Rgb kMuted{0x6A, 0x4F, 0x33};      // status, hints, notes
inline constexpr Rgb kAccent{0x7A, 0x1F, 0x1A};     // buttons, labels
inline constexpr Rgb kAccentHover{0x92, 0x2A, 0x22};
inline constexpr Rgb kAccentActive{0x5E, 0x16, 0x12};
inline constexpr Rgb kOnAccent{0xFB, 0xF5, 0xE6};  // button text
inline constexpr Rgb kError{0x8B, 0x1E, 0x16};     // an error note
inline constexpr Rgb kRule{0xA8, 0x8B, 0x5E};      // divider lines, card edges
inline constexpr Rgb kFrame{0x8F, 0x73, 0x50};     // the frame of the input box and buttons
inline constexpr Rgb kDisabled{0xD6, 0xC6, 0xA4};  // a disabled button
inline constexpr Rgb kDisabledText{0x4A, 0x3A, 0x28};
}  // namespace palette

}  // namespace curia::ui
