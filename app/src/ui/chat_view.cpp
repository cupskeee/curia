// SPDX-License-Identifier: MIT
#include "ui/chat_view.h"

#include <imgui.h>
#include <imgui_stdlib.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <utility>

#include "chatview/view_text.h"

namespace curia::ui {

namespace {

using chatview::AnswerState;
using chatview::ItemKind;
using chatview::TextLayout;
using chatview::Tone;
using chatview::ViewItem;
namespace text = chatview::text;

// The height of a line of text as a multiple of the font size (a book face reads better with
// some air between the lines).
constexpr float kLineSpacing = 1.28F;
// The input box shows this many lines; more scroll inside it.
constexpr float kInputLines = 3.0F;
// One period of the thinking animation, in seconds.
constexpr double kPulseSeconds = 1.6;
// The content column is at most this many font sizes wide (about 90 characters of an answer in
// the bundled face, as at the default window width), so that lines stay easy to read in a wide
// window.
constexpr float kMaxColumnEm = 29.0F;
// The text in the middle of an empty conversation wraps at this many font sizes.
constexpr float kEmptyStateEm = 18.0F;
// The status line and the notices take at most this share of the height left above the input
// box (and at least three lines); more scroll in their own area.
constexpr float kMaxTopShare = 0.35F;

ImVec2 plus(ImVec2 a, ImVec2 b) {
    return {a.x + b.x, a.y + b.y};
}

std::string label(std::string_view value) {
    return std::string(value);
}

// A button in the accent colour. A disabled one has its own colours instead of a faded look, so
// its text stays readable.
bool accentButton(const std::string& caption, bool enabled, ImVec2 size) {
    namespace p = palette;
    if (enabled) {
        ImGui::PushStyleColor(ImGuiCol_Text, p::kOnAccent.vec4());
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, p::kDisabledText.vec4());
        ImGui::PushStyleColor(ImGuiCol_Button, p::kDisabled.vec4());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p::kDisabled.vec4());
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, p::kDisabled.vec4());
        ImGui::BeginDisabled();
    }
    const bool pressed = ImGui::Button(caption.c_str(), size);
    if (enabled) {
        ImGui::PopStyleColor(1);
    } else {
        ImGui::EndDisabled();
        ImGui::PopStyleColor(4);
    }
    return pressed && enabled;
}

// A quieter button (Copy): the answer's own background, ink text, a thin border.
bool quietButton(const std::string& caption) {
    namespace p = palette;
    ImGui::PushStyleColor(ImGuiCol_Text, p::kInk.vec4());
    ImGui::PushStyleColor(ImGuiCol_Button, p::kAnswer.vec4());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, p::kQuestion.vec4());
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, p::kRule.vec4(0.6F));
    const bool pressed = ImGui::Button(caption.c_str());
    ImGui::PopStyleColor(4);
    return pressed;
}

}  // namespace

ChatViewUi::ChatViewUi(FontSet fonts) : fonts_(fonts) {}

void ChatViewUi::replaceDraft(std::string text) {
    draft_ = std::move(text);
    if (inputActive_) {
        // ImGui would keep showing its own copy of the old text. (Enter and a click on Send end
        // the editing first, so a question sent the usual way needs no new box.)
        ++inputGeneration_;
    }
    focusInput_ = true;
}

float ChatViewUi::measure(std::string_view value, bool bold) const {
    if (value.empty()) {
        return 0.0F;
    }
    ImFont* font = bold ? fonts_.bold : fonts_.regular;
    return font
        ->CalcTextSizeA(ImGui::GetFontSize(), FLT_MAX, 0.0F, value.data(),
                        value.data() + value.size())
        .x;
}

TextLayout ChatViewUi::layout(const chatview::AnswerText& value, float width) const {
    return chatview::layoutText(
        value, width, [this](std::string_view piece, bool bold) { return measure(piece, bold); });
}

float ChatViewUi::lineHeight() const {
    return std::floor(ImGui::GetFontSize() * kLineSpacing);
}

float ChatViewUi::blockHeight(const TextLayout& value) const {
    return static_cast<float>(value.lines.size()) * lineHeight();
}

void ChatViewUi::drawText(const TextLayout& value, ImVec2 origin, ImU32 color,
                          ImU32 boldColor) const {
    ImDrawList* list = ImGui::GetWindowDrawList();
    const float size = ImGui::GetFontSize();
    const float line = lineHeight();
    const float lead = std::floor((line - size) * 0.5F);
    for (std::size_t i = 0; i < value.lines.size(); ++i) {
        const float y = origin.y + static_cast<float>(i) * line + lead;
        if (!ImGui::IsRectVisible(ImVec2(origin.x, y), ImVec2(origin.x + 1.0F, y + line))) {
            continue;
        }
        for (const chatview::LayoutSegment& segment : value.lines[i].segments) {
            const std::string& piece = segment.text;
            list->AddText(segment.bold ? fonts_.bold : fonts_.regular, size,
                          ImVec2(std::floor(origin.x + segment.x), y),
                          segment.bold ? boldColor : color, piece.data(),
                          piece.data() + piece.size());
        }
    }
}

namespace {

struct BoxMetrics {
    ImVec2 pad;
    // The accent bar of a warning (0 for an information).
    float bar = 0;
};

BoxMetrics boxMetrics(bool warning) {
    const float size = ImGui::GetFontSize();
    BoxMetrics metrics;
    metrics.pad = ImVec2(std::floor(size * 0.6F), std::floor(size * 0.35F));
    metrics.bar = warning ? std::max(3.0F, std::floor(size * 0.2F)) : 0.0F;
    return metrics;
}

}  // namespace

float ChatViewUi::boxHeight(const std::string& value, float width, bool warning) const {
    const BoxMetrics metrics = boxMetrics(warning);
    const TextLayout lines =
        layout(chatview::plainLines(value), width - 2.0F * metrics.pad.x - metrics.bar);
    return blockHeight(lines) + 2.0F * metrics.pad.y;
}

void ChatViewUi::drawBox(const std::string& value, float width, Rgb background,
                         bool warning) const {
    namespace p = palette;
    const BoxMetrics metrics = boxMetrics(warning);
    const TextLayout lines =
        layout(chatview::plainLines(value), width - 2.0F * metrics.pad.x - metrics.bar);
    const float height = blockHeight(lines) + 2.0F * metrics.pad.y;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 max = plus(pos, ImVec2(width, height));
    const float rounding = ImGui::GetStyle().FrameRounding;
    ImDrawList* list = ImGui::GetWindowDrawList();
    list->AddRectFilled(pos, max, background.u32(), rounding);
    if (warning) {
        // The left edge of the same rounded box in the accent colour, so the bar follows its
        // corners: a warning is told from an information by more than a shade.
        list->PushClipRect(pos, ImVec2(pos.x + metrics.bar, max.y), true);
        list->AddRectFilled(pos, max, p::kAccent.u32(), rounding);
        list->PopClipRect();
    }
    drawText(lines, plus(pos, ImVec2(metrics.pad.x + metrics.bar, metrics.pad.y)), p::kInk.u32());
    ImGui::Dummy(ImVec2(width, height));
}

void ChatViewUi::drawPulse(ImVec2 centre) const {
    namespace p = palette;
    const double phase = std::fmod(ImGui::GetTime(), kPulseSeconds) / kPulseSeconds;
    const float pulse = 0.5F + 0.5F * static_cast<float>(std::sin(phase * 2.0 * 3.14159265));
    const auto alpha = static_cast<std::uint8_t>(90.0F + 165.0F * pulse);
    const float radius = std::floor(ImGui::GetFontSize() * 0.2F);
    ImGui::GetWindowDrawList()->AddCircleFilled(centre, radius, p::kAccent.u32(alpha));
}

void ChatViewUi::drawTop(const chatview::ChatView& view, float width) {
    namespace p = palette;
    if (!view.status.empty()) {
        if (view.statusTone == Tone::Warning) {
            drawBox(view.status, width, p::kNotice, true);
        } else {
            const TextLayout lines = layout(chatview::plainLines(view.status), width);
            drawText(lines, ImGui::GetCursorScreenPos(), p::kMuted.u32());
            ImGui::Dummy(ImVec2(width, blockHeight(lines)));
        }
    }
    for (const chatview::Banner& banner : view.banners) {
        const bool warning = banner.tone == Tone::Warning;
        drawBox(banner.text, width, warning ? p::kNotice : p::kInfo, warning);
    }
}

float ChatViewUi::topHeight(const chatview::ChatView& view, float width) const {
    const float spacing = ImGui::GetStyle().ItemSpacing.y;
    float height = 0;
    if (!view.status.empty()) {
        height += spacing + (view.statusTone == Tone::Warning
                                 ? boxHeight(view.status, width, true)
                                 : blockHeight(layout(chatview::plainLines(view.status), width)));
    }
    for (const chatview::Banner& banner : view.banners) {
        height += spacing + boxHeight(banner.text, width, banner.tone == Tone::Warning);
    }
    return height;
}

void ChatViewUi::drawQuestion(const ViewItem& item) {
    namespace p = palette;
    const float size = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 pad(std::floor(size * 0.6F), std::floor(size * 0.4F));
    // Questions sit to the right, answers span the width: the two are easy to tell apart.
    const float maxCard = std::max(avail - std::floor(size * 2.5F), avail * 0.6F);
    const TextLayout lines = layout(chatview::plainLines(item.text), maxCard - 2.0F * pad.x);
    const float labelWidth = measure(text::kYou, false);
    const float cardWidth =
        std::min(maxCard, std::max(lines.width, labelWidth) + 2.0F * pad.x + 1.0F);
    const float cardHeight = blockHeight(lines) + 2.0F * pad.y;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float labelHeight = lineHeight();
    const float left = pos.x + avail - cardWidth;

    ImDrawList* list = ImGui::GetWindowDrawList();
    const std::string you = label(text::kYou);
    list->AddText(fonts_.regular, size, ImVec2(std::floor(pos.x + avail - labelWidth), pos.y),
                  p::kMuted.u32(), you.c_str());
    const ImVec2 cardMin(left, pos.y + labelHeight);
    list->AddRectFilled(cardMin, plus(cardMin, ImVec2(cardWidth, cardHeight)), p::kQuestion.u32(),
                        ImGui::GetStyle().ChildRounding);
    drawText(lines, plus(cardMin, pad), p::kInk.u32());
    ImGui::Dummy(ImVec2(avail, labelHeight + cardHeight));
}

void ChatViewUi::drawAnswer(const ViewItem& item, ChatActions& actions) {
    namespace p = palette;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float size = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 pad(std::floor(size * 0.7F), std::floor(size * 0.5F));
    const float inner = avail - 2.0F * pad.x;
    const float gap = std::floor(size * 0.35F);
    const bool thinking = item.state == AnswerState::Thinking;

    const TextLayout body = thinking ? TextLayout{} : layout(item.answer, inner);
    const TextLayout note =
        item.note.empty() ? TextLayout{} : layout(chatview::plainLines(item.note), inner);
    const bool buttons = item.offerCopy || item.offerRetry;
    // While the answer arrives, a pulsing dot after its last word.
    const bool arriving = item.state == AnswerState::Streaming;
    float dotAfter = 0;
    if (arriving && !body.lines.empty() && !body.lines.back().segments.empty()) {
        const chatview::LayoutSegment& last = body.lines.back().segments.back();
        dotAfter = last.x + last.width;
    }
    const bool dotOwnLine =
        arriving && (body.lines.empty() || dotAfter + 4.0F * std::floor(size * 0.2F) > inner);

    float contentHeight = thinking ? lineHeight() : blockHeight(body);
    if (dotOwnLine) {
        contentHeight += lineHeight();
    }
    if (!note.lines.empty()) {
        contentHeight += (contentHeight > 0.0F ? gap : 0.0F) + blockHeight(note);
    }
    if (buttons) {
        contentHeight += gap + ImGui::GetFrameHeight();
    }
    const float labelHeight = lineHeight();
    const float cardHeight = contentHeight + 2.0F * pad.y;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImDrawList* list = ImGui::GetWindowDrawList();

    const std::string advisor = label(text::kAdvisor);
    list->AddText(fonts_.bold, size, pos, p::kAccent.u32(), advisor.c_str());
    const ImVec2 cardMin(pos.x, pos.y + labelHeight);
    const ImVec2 cardMax = plus(cardMin, ImVec2(avail, cardHeight));
    list->AddRectFilled(cardMin, cardMax, p::kAnswer.u32(), style.ChildRounding);
    list->AddRect(cardMin, cardMax, p::kRule.u32(110), style.ChildRounding);

    ImVec2 cursor = plus(cardMin, pad);
    const float radius = std::floor(size * 0.2F);
    if (thinking) {
        // The pulsing dot beside the word.
        drawPulse(ImVec2(cursor.x + radius, cursor.y + lineHeight() * 0.5F));
        const std::string word = label(text::kThinking);
        list->AddText(
            fonts_.regular, size,
            ImVec2(cursor.x + 3.0F * radius, cursor.y + std::floor((lineHeight() - size) * 0.5F)),
            p::kMuted.u32(), word.c_str());
        cursor.y += lineHeight();
    } else {
        drawText(body, cursor, p::kInk.u32());
        cursor.y += blockHeight(body);
        if (arriving) {
            // More is coming: the dot after the last word (or on a line of its own).
            const float x = dotOwnLine ? radius : dotAfter + 2.0F * radius;
            const float y =
                dotOwnLine ? cursor.y + lineHeight() * 0.5F : cursor.y - lineHeight() * 0.5F;
            drawPulse(ImVec2(cursor.x + x, y));
            if (dotOwnLine) {
                cursor.y += lineHeight();
            }
        }
    }
    if (!note.lines.empty()) {
        if (cursor.y > cardMin.y + pad.y) {
            cursor.y += gap;
        }
        drawText(note, cursor, item.noteIsError ? p::kError.u32() : p::kMuted.u32());
        cursor.y += blockHeight(note);
    }
    if (buttons) {
        cursor.y += gap;
        ImGui::SetCursorScreenPos(cursor);
        ImGui::PushID(static_cast<int>(item.entry));
        if (item.offerCopy) {
            const bool copiedHere = copied_.has_value() && *copied_ == item.entry;
            if (quietButton(label(copiedHere ? text::kCopied : text::kCopy) + "###copy")) {
                actions.copy = item.entry;
            }
            if (item.offerRetry) {
                ImGui::SameLine();
            }
        }
        if (item.offerRetry && accentButton(label(text::kRetry), true, ImVec2(0.0F, 0.0F))) {
            actions.retry = true;
        }
        ImGui::PopID();
    }
    // One item over the whole row, so the conversation knows its height.
    ImGui::SetCursorScreenPos(pos);
    ImGui::Dummy(ImVec2(avail, labelHeight + cardHeight));
}

void ChatViewUi::drawDivider(const ViewItem& item) {
    namespace p = palette;
    const float size = ImGui::GetFontSize();
    const float avail = ImGui::GetContentRegionAvail().x;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float textWidth = measure(item.text, false);
    const float minLine = std::floor(size * 1.2F);
    const float gap = std::floor(size * 0.5F);
    ImDrawList* list = ImGui::GetWindowDrawList();
    if (textWidth + 2.0F * (minLine + gap) <= avail) {
        const float left = pos.x + std::floor((avail - textWidth) * 0.5F);
        const float midY = std::floor(pos.y + lineHeight() * 0.5F) + 0.5F;
        list->AddLine(ImVec2(pos.x, midY), ImVec2(left - gap, midY), p::kRule.u32(), 1.0F);
        list->AddLine(ImVec2(left + textWidth + gap, midY), ImVec2(pos.x + avail, midY),
                      p::kRule.u32(), 1.0F);
        list->AddText(fonts_.regular, size,
                      ImVec2(left, pos.y + std::floor((lineHeight() - size) * 0.5F)),
                      p::kMuted.u32(), item.text.c_str());
        ImGui::Dummy(ImVec2(avail, lineHeight()));
        return;
    }
    // Too narrow for lines on both sides: a rule above the wrapped label.
    const TextLayout lines = layout(chatview::plainLines(item.text), avail);
    const float ruleY = std::floor(pos.y + gap * 0.5F) + 0.5F;
    list->AddLine(ImVec2(pos.x, ruleY), ImVec2(pos.x + avail, ruleY), p::kRule.u32(), 1.0F);
    drawText(lines, ImVec2(pos.x, pos.y + gap), p::kMuted.u32());
    ImGui::Dummy(ImVec2(avail, gap + blockHeight(lines)));
}

void ChatViewUi::drawEmptyState(const std::string& value, float height) {
    namespace p = palette;
    const float avail = ImGui::GetContentRegionAvail().x;
    const TextLayout lines =
        layout(chatview::plainLines(value),
               std::min(avail, std::floor(ImGui::GetFontSize() * kEmptyStateEm)));
    const float textHeight = blockHeight(lines);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // A little above the middle, each line centred, in ink: the first thing to read.
    const float top = std::max(0.0F, std::floor((height - textHeight) * 0.4F));
    for (std::size_t i = 0; i < lines.lines.size(); ++i) {
        const chatview::LayoutLine& line = lines.lines[i];
        const float width =
            line.segments.empty() ? 0.0F : line.segments.back().x + line.segments.back().width;
        TextLayout one;
        one.lines.push_back(line);
        drawText(one,
                 ImVec2(pos.x + std::floor((avail - width) * 0.5F),
                        pos.y + top + static_cast<float>(i) * lineHeight()),
                 p::kInk.u32());
    }
    ImGui::Dummy(ImVec2(avail, top + textHeight));
}

void ChatViewUi::drawConversation(const chatview::ChatView& view, float height,
                                  ChatActions& actions) {
    if (view.items.size() != lastItemCount_) {
        if (view.items.size() > lastItemCount_) {
            copied_.reset();  // a new row: the acknowledgement has done its job
        }
        lastItemCount_ = view.items.size();
    }
    ImGui::BeginChild("##conversation", ImVec2(0.0F, height), ImGuiChildFlags_None,
                      ImGuiWindowFlags_None);
    if (view.items.empty() && !view.emptyState.empty()) {
        drawEmptyState(view.emptyState, height);
    }
    const float spacing = std::floor(ImGui::GetFontSize() * 0.55F);
    for (std::size_t i = 0; i < view.items.size(); ++i) {
        const ViewItem& item = view.items[i];
        if (i > 0) {
            ImGui::Dummy(ImVec2(0.0F, spacing));
        }
        switch (item.kind) {
        case ItemKind::Question:
            drawQuestion(item);
            break;
        case ItemKind::Answer:
            drawAnswer(item, actions);
            break;
        case ItemKind::Divider:
            drawDivider(item);
            break;
        }
    }
    // Follow the newest text unless the player has scrolled up; back at the bottom, follow again.
    // (Content that grows never moves the scroll position up; only the player does.)
    const float scrollY = ImGui::GetScrollY();
    if (scrollY < lastScrollY_ - 0.5F) {
        following_ = false;
    }
    if (scrollY >= ImGui::GetScrollMaxY() - 1.0F) {
        following_ = true;
    }
    if (following_) {
        ImGui::SetScrollHereY(1.0F);
    }
    lastScrollY_ = scrollY;
    if (scrollY > 0.5F) {
        // Scrolled down: a thin rule along the top edge, so that a row cut off there reads as
        // "more above" rather than a drawing fault.
        const ImVec2 min = ImGui::GetWindowPos();
        const float y = std::floor(min.y) + 0.5F;
        ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y),
                                            ImVec2(min.x + ImGui::GetWindowWidth(), y),
                                            palette::kRule.u32(), 1.0F);
    }
    ImGui::EndChild();
}

float ChatViewUi::bottomHeight(const chatview::ChatView& view) const {
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;
    const TextLayout reminder = layout(view.pauseReminder, width);
    const float input =
        std::floor(ImGui::GetFontSize() * kInputLines) + 2.0F * style.FramePadding.y;
    return blockHeight(reminder) + style.ItemSpacing.y + input;
}

void ChatViewUi::drawBottom(const chatview::ChatView& view, ChatActions& actions) {
    namespace p = palette;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float width = ImGui::GetContentRegionAvail().x;

    // The pause reminder (FR-OVL-10), right above the box the player types into; its lead in bold
    // ink stands out from the other muted notes.
    const TextLayout reminder = layout(view.pauseReminder, width);
    drawText(reminder, ImGui::GetCursorScreenPos(), p::kMuted.u32(), p::kInk.u32());
    ImGui::Dummy(ImVec2(width, blockHeight(reminder)));

    const std::string send = label(text::kSend);
    const std::string stop = label(text::kStop);
    const float buttonWidth = std::floor(std::max(measure(send, false), measure(stop, false)) +
                                         3.0F * style.FramePadding.x);
    const float inputHeight =
        std::floor(ImGui::GetFontSize() * kInputLines) + 2.0F * style.FramePadding.y;
    const float inputWidth = std::max(1.0F, width - buttonWidth - style.ItemSpacing.x);

    if (focusInput_) {
        ImGui::SetKeyboardFocusHere();
        focusInput_ = false;
    }
    const ImGuiInputTextFlags flags = ImGuiInputTextFlags_CtrlEnterForNewLine |
                                      ImGuiInputTextFlags_EnterReturnsTrue |
                                      ImGuiInputTextFlags_WordWrap;
    ImGui::PushID(inputGeneration_);
    const bool entered =
        ImGui::InputTextMultiline("##ask", &draft_, ImVec2(inputWidth, inputHeight), flags);
    // After Enter or a click elsewhere (on Send) ImGui has already ended the editing here.
    inputActive_ = ImGui::IsItemActive();
    ImGui::PopID();
    if (draft_.empty()) {
        // The hint, drawn over the box (its background belongs to a child window, which would
        // cover anything drawn in this window's list).
        const ImVec2 min = ImGui::GetItemRectMin();
        const ImVec2 max = ImGui::GetItemRectMax();
        const ImVec2 at = plus(min, style.FramePadding);
        const TextLayout hint =
            layout(chatview::plainLines(view.inputHint), inputWidth - 2.0F * style.FramePadding.x);
        ImDrawList* foreground = ImGui::GetForegroundDrawList();
        foreground->PushClipRect(min, max, true);
        const float size = ImGui::GetFontSize();
        for (std::size_t i = 0; i < hint.lines.size(); ++i) {
            for (const chatview::LayoutSegment& segment : hint.lines[i].segments) {
                foreground->AddText(fonts_.regular, size,
                                    ImVec2(at.x + segment.x, at.y + static_cast<float>(i) * size),
                                    p::kMuted.u32(), segment.text.c_str());
            }
        }
        foreground->PopClipRect();
    }
    if (entered) {
        // Enter ends the editing in ImGui: give the box its focus back. Whether anything is
        // sent is the controller's call (blank, busy and not configured are refused there).
        actions.send = true;
        focusInput_ = true;
    }
    ImGui::SameLine();
    if (view.canStop) {
        if (accentButton(stop, true, ImVec2(buttonWidth, inputHeight))) {
            actions.stop = true;
        }
    } else if (accentButton(send, view.canSend, ImVec2(buttonWidth, inputHeight))) {
        actions.send = true;
        focusInput_ = true;
    }
}

ChatActions ChatViewUi::draw(const chatview::ChatView& view) {
    ChatActions actions;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImGuiStyle& style = ImGui::GetStyle();
    // A centred column in a wide window (the parchment of the clear colour fills the sides).
    const float column =
        std::min(viewport->WorkSize.x,
                 std::floor(ImGui::GetFontSize() * kMaxColumnEm) + 2.0F * style.WindowPadding.x);
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + std::floor((viewport->WorkSize.x - column) * 0.5F),
               viewport->WorkPos.y));
    ImGui::SetNextWindowSize(ImVec2(column, viewport->WorkSize.y));
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::Begin("##curia", nullptr, flags);
    // ImGui gives the frame's rasterizer density (2 on a Retina display) only to the font that is
    // pushed; the bold face is measured and drawn without being pushed, so it would be baked at
    // density 1 and scaled up (blurry). Pushing it once here hands it the density for this frame.
    ImGui::PushFont(fonts_.bold, 0.0F);
    ImGui::PopFont();

    // Esc must not throw the typed question away (ImGui's text box reverts on Esc).
    // TODO(PR 8): Esc hides the overlay and returns the keys to CK3.
    ImGui::Shortcut(ImGuiKey_Escape, ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive);
    // Cmd+. stops the answer on macOS (FR-OVL-4); ImGuiMod_Ctrl is Cmd there.
    if (view.canStop && ImGui::GetIO().ConfigMacOSXBehaviors &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Period,
                        ImGuiInputFlags_RouteGlobal | ImGuiInputFlags_RouteOverActive)) {
        actions.stop = true;
    }

    const float width = ImGui::GetContentRegionAvail().x;
    const float bottom = bottomHeight(view);
    const float top = topHeight(view, width);
    const float topCap =
        std::max(3.0F * lineHeight(),
                 std::floor((ImGui::GetContentRegionAvail().y - bottom) * kMaxTopShare));
    if (top > topCap) {
        // Many notices in a short window: they scroll in their own area and leave room for the
        // conversation.
        ImGui::BeginChild("##notices", ImVec2(0.0F, topCap), ImGuiChildFlags_None,
                          ImGuiWindowFlags_None);
        drawTop(view, ImGui::GetContentRegionAvail().x);
        if (ImGui::GetScrollY() < ImGui::GetScrollMaxY() - 0.5F) {
            // More below: a rule along the bottom edge (as the conversation has one on top).
            const ImVec2 min = ImGui::GetWindowPos();
            const float y = std::floor(min.y + ImGui::GetWindowHeight()) - 0.5F;
            ImGui::GetWindowDrawList()->AddLine(ImVec2(min.x, y),
                                                ImVec2(min.x + ImGui::GetWindowWidth(), y),
                                                palette::kRule.u32(), 1.0F);
        }
        ImGui::EndChild();
    } else if (top > 0.0F) {
        drawTop(view, width);
    }
    const float available = ImGui::GetContentRegionAvail().y;
    const float conversation =
        std::max(ImGui::GetFontSize() * 2.0F, available - bottom - style.ItemSpacing.y);
    drawConversation(view, conversation, actions);
    drawBottom(view, actions);
    ImGui::End();
    return actions;
}

}  // namespace curia::ui
