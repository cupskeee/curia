// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <string_view>

#include "chatview/view_model.h"
#include "core/app_controller.h"
#include "ui/chat_view.h"
#include "ui/theme.h"

// The chat window on the UI thread: the controller's observer (it learns what changed), the ImGui
// view, and what the run loop needs to know (did something visible change, does the window want
// to come to the front or to be hidden, does something move by itself).
namespace curia::ui {

class ChatWindow final : public AppObserver {
public:
    struct Options {
        // A game log was found (or given with --log); otherwise the status says CK3's folder was
        // not found.
        bool gameFolderFound = true;
        // CK3's user folder is under Documents on this OS (that text then says where to look).
        bool ck3FolderInDocuments = true;
    };

    ChatWindow(AppController& controller, FontSet fonts, Options options);
    ~ChatWindow() override;
    ChatWindow(const ChatWindow&) = delete;
    ChatWindow& operator=(const ChatWindow&) = delete;

    // The window's content for one frame (between the backend's beginFrame() and endFrame()):
    // builds the view model, draws it and carries out what the player did.
    void draw();

    // Sends the text in the input box as a question (Enter and Send do this). True if it was
    // accepted; the box is emptied then.
    bool submit();
    void setDraft(std::string_view text);
    void focusInput() { view_.focusInput(); }

    // Something on screen changed since the last call (the controller told us); resets the flag.
    bool takeChanged();
    // The player clicked Advisor in the game since the last call; resets the flag.
    bool takeOverlayRequest();
    // The player pressed Esc since the last call (hide the overlay); resets the flag.
    bool takeHideRequest();
    // The thinking animation (or the dot of an answer that is arriving) is on: the loop wakes up
    // on the spinner timeout.
    bool spinnerActive() const { return spinnerActive_; }
    // The last view drawn (the smoke test checks it).
    const chatview::ChatView& lastView() const { return lastView_; }

private:
    // AppObserver
    void onChatState(advisor::ChatState state) override;
    void onTranscriptChanged(std::size_t index) override;
    void onSnapshotChanged(const SnapshotInfo& info) override;
    void onOverlayRequested(bool probeExport) override;
    void onNoticeChanged(advisor::Notice notice) override;
    void onUnsupportedFormat(std::string_view leadingFields) override;

    void copyAnswer(std::size_t entry);

    AppController& controller_;
    ChatViewUi view_;
    chatview::WindowFacts facts_;
    chatview::ChatView lastView_;
    bool changed_ = true;
    bool overlayRequested_ = false;
    bool hideRequested_ = false;
    bool spinnerActive_ = false;
};

}  // namespace curia::ui
