// SPDX-License-Identifier: MIT
#include "ui/theme.h"

#include <climits>
#include <cstdio>

#include "ui/fonts.h"

namespace curia::ui {

namespace {

ImFont* addBundledFace(const EmbeddedBytes& bytes, const char* name) {
    if (bytes.data == nullptr || bytes.size == 0 ||
        bytes.size > static_cast<std::size_t>(INT_MAX)) {
        return nullptr;
    }
    ImFontConfig config;
    // The bytes belong to the executable: ImGui must never free them.
    config.FontDataOwnedByAtlas = false;
    std::snprintf(config.Name, sizeof(config.Name), "%s", name);
    // ImGui only reads the data, but its signature takes a non-const pointer.
    void* data = const_cast<unsigned char*>(bytes.data);
    return ImGui::GetIO().Fonts->AddFontFromMemoryTTF(data, static_cast<int>(bytes.size),
                                                      kBaseFontSize, &config);
}

}  // namespace

FontSet loadFonts() {
    FontSet fonts;
    ImFont* regular = addBundledFace(bundledRegularFont(), "Gentium Book Plus");
    ImFont* bold =
        regular != nullptr ? addBundledFace(bundledBoldFont(), "Gentium Book Plus Bold") : nullptr;
    if (regular != nullptr && bold != nullptr) {
        fonts.regular = regular;
        fonts.bold = bold;
        fonts.bundled = true;
        return fonts;
    }
    std::fputs("curia: the bundled font could not be loaded; using the built-in font\n", stderr);
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->Clear();
    ImFont* fallback = atlas->AddFontDefault();
    fonts.regular = fallback;
    fonts.bold = fallback;
    return fonts;
}

void applyTheme(float uiScale) {
    namespace p = palette;
    ImGuiStyle style;
    ImGui::StyleColorsLight(&style);

    style.WindowPadding = ImVec2(14.0F, 12.0F);
    style.FramePadding = ImVec2(10.0F, 6.0F);
    style.ItemSpacing = ImVec2(8.0F, 8.0F);
    style.ItemInnerSpacing = ImVec2(6.0F, 4.0F);
    style.ScrollbarSize = 12.0F;
    style.WindowRounding = 0.0F;
    style.ChildRounding = 6.0F;
    style.FrameRounding = 5.0F;
    style.PopupRounding = 6.0F;
    style.ScrollbarRounding = 6.0F;
    style.GrabRounding = 5.0F;
    style.WindowBorderSize = 0.0F;
    style.ChildBorderSize = 0.0F;
    style.FrameBorderSize = 1.0F;
    // Disabled buttons get their own colours (chat_view.cpp), not a faded look that would lower
    // the contrast of their text.
    style.DisabledAlpha = 1.0F;

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_Text] = p::kInk.vec4();
    colors[ImGuiCol_TextDisabled] = p::kMuted.vec4();
    colors[ImGuiCol_WindowBg] = p::kParchment.vec4();
    colors[ImGuiCol_ChildBg] = p::kParchment.vec4(0.0F);
    colors[ImGuiCol_PopupBg] = p::kAnswer.vec4();
    colors[ImGuiCol_Border] = p::kFrame.vec4();
    colors[ImGuiCol_BorderShadow] = p::kInk.vec4(0.0F);
    colors[ImGuiCol_FrameBg] = p::kInput.vec4();
    colors[ImGuiCol_FrameBgHovered] = p::kInput.vec4();
    colors[ImGuiCol_FrameBgActive] = p::kInput.vec4();
    colors[ImGuiCol_ScrollbarBg] = p::kParchment.vec4(0.0F);
    colors[ImGuiCol_ScrollbarGrab] = p::kRule.vec4(0.55F);
    colors[ImGuiCol_ScrollbarGrabHovered] = p::kRule.vec4(0.8F);
    colors[ImGuiCol_ScrollbarGrabActive] = p::kRule.vec4();
    colors[ImGuiCol_Button] = p::kAccent.vec4();
    colors[ImGuiCol_ButtonHovered] = p::kAccentHover.vec4();
    colors[ImGuiCol_ButtonActive] = p::kAccentActive.vec4();
    colors[ImGuiCol_Header] = p::kQuestion.vec4();
    colors[ImGuiCol_HeaderHovered] = p::kQuestion.vec4();
    colors[ImGuiCol_HeaderActive] = p::kQuestion.vec4();
    colors[ImGuiCol_Separator] = p::kRule.vec4();
    colors[ImGuiCol_TextSelectedBg] = p::kAccent.vec4(0.25F);
    colors[ImGuiCol_InputTextCursor] = p::kInk.vec4();
    colors[ImGuiCol_NavCursor] = p::kAccent.vec4();

    style.ScaleAllSizes(uiScale);
    style.FontSizeBase = kBaseFontSize;
    style.FontScaleDpi = uiScale;
    ImGui::GetStyle() = style;
}

}  // namespace curia::ui
