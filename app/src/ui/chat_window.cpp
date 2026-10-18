// SPDX-License-Identifier: MIT
#include "ui/chat_window.h"

#include <SDL3/SDL.h>

#include <string>

#include "advisor/transcript.h"
#include "chatview/answer_text.h"

namespace curia::ui {

ChatWindow::ChatWindow(AppController& controller, FontSet fonts, Options options)
    : controller_(controller), view_(fonts) {
    facts_.gameFolderFound = options.gameFolderFound;
    facts_.ck3FolderInDocuments = options.ck3FolderInDocuments;
    controller_.setObserver(this);
}

ChatWindow::~ChatWindow() {
    controller_.setObserver(nullptr);
}

void ChatWindow::draw() {
    facts_.draftEmpty = chatview::isBlankText(view_.draft());
    // The whole conversation is parsed and laid out again for every frame (measured: about 0.4 ms
    // for 20 rows, 1.9 ms for 100 on an arm64 Mac). TODO: keep the parsed and laid-out rows of the
    // finished answers once sessions get long enough for it to show.
    lastView_ = chatview::buildChatView(chatview::viewInputs(controller_, facts_));
    spinnerActive_ = lastView_.spinnerActive;
    const ChatActions actions = view_.draw(lastView_);
    if (actions.stop) {
        controller_.onStop();
    }
    if (actions.retry) {
        controller_.onRetry();
    }
    if (actions.send && !chatview::isBlankText(view_.draft())) {
        submit();  // Enter in an empty box sends nothing (and logs nothing)
    }
    if (actions.copy.has_value()) {
        copyAnswer(*actions.copy);
    }
}

bool ChatWindow::submit() {
    if (controller_.onUserAsk(view_.draft()) != advisor::AskOutcome::Started) {
        return false;  // blank, busy or not configured: the text stays in the box
    }
    view_.replaceDraft({});
    changed_ = true;
    return true;
}

void ChatWindow::setDraft(std::string_view text) {
    view_.replaceDraft(std::string(text));
    changed_ = true;
}

void ChatWindow::copyAnswer(std::size_t entry) {
    const std::vector<advisor::TranscriptEntry>& transcript = controller_.session().transcript();
    if (entry >= transcript.size() || transcript[entry].kind != advisor::EntryKind::Assistant) {
        return;
    }
    // What the row shows, without its markers (and checked for secrets, as the row is).
    const std::string plain = chatview::plainText(
        chatview::shownAnswer(transcript[entry].text, true, controller_.registry()));
    // The clipboard belongs to the UI thread (SDL).
    if (SDL_SetClipboardText(plain.c_str())) {
        view_.markCopied(entry);
    }
    changed_ = true;
}

bool ChatWindow::takeChanged() {
    const bool changed = changed_;
    changed_ = false;
    return changed;
}

bool ChatWindow::takeOverlayRequest() {
    const bool requested = overlayRequested_;
    overlayRequested_ = false;
    return requested;
}

void ChatWindow::onChatState(advisor::ChatState /*state*/) {
    changed_ = true;
}

void ChatWindow::onTranscriptChanged(std::size_t /*index*/) {
    changed_ = true;
}

void ChatWindow::onSnapshotChanged(const SnapshotInfo& info) {
    facts_.onSnapshotChanged(info);
    changed_ = true;
}

void ChatWindow::onOverlayRequested(bool /*probeExport*/) {
    overlayRequested_ = true;
    changed_ = true;
}

void ChatWindow::onNoticeChanged(advisor::Notice /*notice*/) {
    changed_ = true;
}

void ChatWindow::onUnsupportedFormat(std::string_view /*leadingFields*/) {
    facts_.onUnsupportedFormat();
    changed_ = true;
}

}  // namespace curia::ui
