// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

#include "chatview/answer_text.h"
#include "chatview/text_layout.h"
#include "chatview/view_model.h"
#include "ui/theme.h"

// Draws a chatview::ChatView with Dear ImGui over the whole window: the status line and notices at
// the top, the conversation in a scrolling area, the pause reminder and the input box with its
// button at the bottom. In a wide window the content is a centred column, so that lines keep a
// readable length. It keeps only what belongs to the screen (the typed text, the scroll position,
// which answer was just copied); everything else comes from the view model. UI thread only,
// between the render backend's beginFrame() and endFrame().
namespace curia::ui {

// What the player did in one frame.
struct ChatActions {
    // Send was clicked or Enter pressed while sending is possible; the text is draft().
    bool send = false;
    // Stop was clicked, or Cmd+. pressed on macOS.
    bool stop = false;
    bool retry = false;
    // Esc was pressed: hide the overlay (the typed text stays in the box).
    bool hide = false;
    // Copy was clicked on the answer of this transcript entry.
    std::optional<std::size_t> copy;
};

class ChatViewUi {
public:
    explicit ChatViewUi(FontSet fonts);

    ChatActions draw(const chatview::ChatView& view);

    // The text in the input box (what the player typed).
    const std::string& draft() const { return draft_; }
    // Puts a text into the input box (also "" after a question was sent); the box gets the focus.
    // While the box is being edited ImGui keeps its own copy of the text, so then the box is
    // replaced by a fresh one (a new ImGui id; each one leaves a small window behind in ImGui, so
    // this happens only when needed).
    void replaceDraft(std::string text);
    // The input box takes the keyboard focus on the next frame.
    void focusInput() { focusInput_ = true; }
    // The Copy button of this entry says "Copied" until the next copy or a new row.
    void markCopied(std::size_t entry) { copied_ = entry; }

private:
    float measure(std::string_view text, bool bold) const;
    chatview::TextLayout layout(const chatview::AnswerText& text, float width) const;
    // The em of the text in pixels (the font size ImGui works with is larger: ui/theme.h).
    float em() const;
    // The distance from one line of text to the next.
    float lineHeight() const;
    // How much height a laid-out line takes: a line of text, or a blank line of the text (a
    // paragraph gap, shorter).
    float rowHeight(const chatview::LayoutLine& line) const;
    float blockHeight(const chatview::TextLayout& layout) const;
    // Draws a laid-out text with its top left corner at `origin`; bold segments in `boldColor`.
    void drawText(const chatview::TextLayout& layout, ImVec2 origin, ImU32 color,
                  ImU32 boldColor) const;
    void drawText(const chatview::TextLayout& layout, ImVec2 origin, ImU32 color) const {
        drawText(layout, origin, color, color);
    }
    // A notice: text in a filled, rounded box across `width`; a warning has a bar in the accent
    // colour on its left edge (an information has none).
    void drawBox(const std::string& text, float width, Rgb background, bool warning) const;
    // A small dot that slowly brightens and fades (the thinking and arriving states): the frames
    // come from the spinner timeout of core/ui_pacing.h, nothing loops.
    void drawPulse(ImVec2 centre) const;

    // The status line and the notices, each as tall as its text needs.
    void drawTop(const chatview::ChatView& view, float width);
    void drawConversation(const chatview::ChatView& view, float height, ChatActions& actions);
    void drawEmptyState(const std::string& text, float height);
    void drawQuestion(const chatview::ViewItem& item);
    void drawAnswer(const chatview::ViewItem& item, ChatActions& actions);
    void drawDivider(const chatview::ViewItem& item);
    void drawBottom(const chatview::ChatView& view, ChatActions& actions);
    float bottomHeight(const chatview::ChatView& view) const;

    FontSet fonts_;
    std::string draft_;
    // Part of the input box's ImGui id: a new number gives a fresh box (replaceDraft()).
    int inputGeneration_ = 0;
    // The input box was being edited in the last frame drawn.
    bool inputActive_ = false;
    bool focusInput_ = false;
    std::optional<std::size_t> copied_;
    std::size_t lastItemCount_ = 0;
    // Auto-scroll: the view follows the newest text until the player scrolls up, and again once
    // they are back at the bottom.
    bool following_ = true;
    float lastScrollY_ = 0;
};

}  // namespace curia::ui
