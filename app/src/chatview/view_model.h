// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/chat_session.h"
#include "advisor/log_silence_monitor.h"
#include "advisor/transcript.h"
#include "bridge/frame.h"
#include "chatview/answer_text.h"
#include "core/app_controller.h"
#include "core/redact.h"

// What the chat window draws, computed from the controller's state by a pure function: the window
// code (ui/chat_view.cpp) only lays it out with ImGui. The texts are those of view_text.h and the
// advisor's own (errors, dividers, not configured).
namespace curia::chatview {

enum class Tone { Plain, Warning };

struct Banner {
    std::string text;
    Tone tone = Tone::Warning;
};

enum class ItemKind { Question, Answer, Divider };

enum class AnswerState {
    // Waiting for the first word (text::kThinking, with a gentle animation).
    Thinking,
    Streaming,
    Complete,
    Stopped,
    Failed,
};

struct ViewItem {
    ItemKind kind = ItemKind::Question;
    // The transcript entry the row shows.
    std::size_t entry = 0;
    // A question or a divider: its text.
    std::string text;
    // An answer: the text as shown (empty while thinking).
    AnswerText answer;
    AnswerState state = AnswerState::Complete;
    // An answer: a line under it (stopped, the error, the length limit); empty = none.
    std::string note;
    bool noteIsError = false;
    // Copy: on every answer that is no longer arriving and has text (complete, stopped or failed
    // part way: the part that came can still be worth keeping).
    bool offerCopy = false;
    bool offerRetry = false;
};

struct ChatView {
    // Empty = no status line (the empty conversation says it instead).
    std::string status;
    Tone statusTone = Tone::Plain;
    std::vector<Banner> banners;
    std::vector<ViewItem> items;
    // Shown in the middle of the conversation while it has no rows; empty = nothing.
    std::string emptyState;
    // Its lead is bold.
    AnswerText pauseReminder;
    std::string inputHint;
    // Send (the button and Enter): configured, nothing in flight and a question typed.
    bool canSend = false;
    // Stop (the button and Cmd+. on macOS): a question is in flight.
    bool canStop = false;
    // Something on screen moves by itself (the thinking animation): the window redraws on a timer
    // (core/ui_pacing.h).
    bool spinnerActive = false;
};

// Everything the view depends on.
struct ViewInputs {
    // Not owned; null = an empty conversation.
    const std::vector<advisor::TranscriptEntry>* transcript = nullptr;
    advisor::ChatState state = advisor::ChatState::Idle;
    bool configured = false;
    // Shown while not configured.
    std::string notConfiguredMessage;
    bool hasSnapshot = false;
    std::optional<bridge::Date> date;
    // The last update from the game could not be read (damaged, or a format this app does not
    // read).
    bool unreadable = false;
    // The newest reading came from the mod's test button.
    bool probe = false;
    // The mod in the game writes a format this app does not read (a version mismatch).
    bool unsupportedFormat = false;
    advisor::Notice notice = advisor::Notice::None;
    // A game log to read was found (or given with --log).
    bool gameFolderFound = true;
    // CK3's user folder is under Documents on this OS (Windows, macOS): the folder-not-found text
    // says where to look.
    bool ck3FolderInDocuments = true;
    // The input box holds only blanks.
    bool draftEmpty = true;
    // The secrets to scrub from what is shown (shownAnswer()); null = the process-wide registry.
    const SecretRegistry* registry = nullptr;
};

ChatView buildChatView(const ViewInputs& inputs);

// An answer as the window shows it and Copy puts it on the clipboard: parseAnswer(), then a last
// check for secrets. The session redacts the raw text, but a key that the model split with markers
// ("sk-ant-**...**", backticks, a link) passes that check and is joined again once the markers are
// removed; if the shown text holds a secret, the answer is shown redacted, as plain lines.
AnswerText shownAnswer(std::string_view text, bool complete, const SecretRegistry* registry);

// What the window remembers between the controller's callbacks, because the controller keeps no
// flag for it.
struct WindowFacts {
    bool unsupportedFormat = false;
    bool gameFolderFound = true;
    bool ck3FolderInDocuments = true;
    bool draftEmpty = true;

    // A readable reading clears an earlier version mismatch.
    void onSnapshotChanged(const SnapshotInfo& info) {
        if (!info.unreadable) {
            unsupportedFormat = false;
        }
    }
    void onUnsupportedFormat() { unsupportedFormat = true; }
};

// The inputs from the controller and the window's own facts.
ViewInputs viewInputs(const AppController& controller, const WindowFacts& facts);

// True if `text` holds nothing but blanks and line breaks (Send stays off).
bool isBlankText(std::string_view text);

}  // namespace curia::chatview
