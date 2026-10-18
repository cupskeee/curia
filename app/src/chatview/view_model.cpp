// SPDX-License-Identifier: MIT
#include "chatview/view_model.h"

#include <algorithm>

#include "advisor/chat_error_text.h"
#include "chatview/view_text.h"

namespace curia::chatview {

namespace {

using advisor::AnswerEnd;
using advisor::EntryKind;
using advisor::TranscriptEntry;

bool inFlight(advisor::ChatState state) {
    return state == advisor::ChatState::Waiting || state == advisor::ChatState::Streaming;
}

// The answer that a retry would replace: the one after the last question (ChatSession::retry()).
std::optional<std::size_t> lastAnswer(const std::vector<TranscriptEntry>& transcript) {
    for (std::size_t i = transcript.size(); i > 0; --i) {
        if (transcript[i - 1].kind == EntryKind::Assistant) {
            return i - 1;
        }
        if (transcript[i - 1].kind == EntryKind::User) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

// While an answer arrives, the end of the shown text may be the start of a key whose rest is still
// to come: redactFinal() cuts that too.
std::string scrub(const std::string& text, bool complete, const SecretRegistry* registry) {
    if (registry != nullptr) {
        return complete ? registry->redact(text) : registry->redactFinal(text);
    }
    return complete ? curia::redact(text) : curia::redactFinal(text);
}

ViewItem answerItem(const TranscriptEntry& entry, std::size_t index, bool retryAllowed,
                    const SecretRegistry* registry) {
    ViewItem item;
    item.kind = ItemKind::Answer;
    item.entry = index;
    const bool pending = entry.end == AnswerEnd::Pending;
    item.answer = shownAnswer(entry.text, !pending, registry);
    switch (entry.end) {
    case AnswerEnd::Pending:
        item.state = item.answer.lines.empty() ? AnswerState::Thinking : AnswerState::Streaming;
        break;
    case AnswerEnd::Complete:
        item.state = AnswerState::Complete;
        if (entry.hitTokenLimit) {
            item.note = std::string(text::kCutShort);
        }
        break;
    case AnswerEnd::Stopped:
        item.state = AnswerState::Stopped;
        item.note = std::string(text::kStopped);
        break;
    case AnswerEnd::Failed:
        item.state = AnswerState::Failed;
        item.note = entry.error.has_value() ? entry.error->summary : std::string();
        if (item.note.empty()) {
            // The session always describes its errors; this is the wording for "Other".
            item.note = advisor::describeChatError(providers::ChatError{}).summary;
        }
        item.noteIsError = true;
        item.offerRetry = retryAllowed && entry.error.has_value() && entry.error->retryable;
        break;
    }
    item.offerCopy = !pending && !item.answer.lines.empty();
    return item;
}

}  // namespace

bool isBlankText(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
}

AnswerText shownAnswer(std::string_view text, bool complete, const SecretRegistry* registry) {
    AnswerText answer = parseAnswer(text, complete);
    const std::string plain = plainText(answer);
    const std::string checked = scrub(plain, complete, registry);
    if (checked != plain) {
        return plainLines(checked);
    }
    return answer;
}

ChatView buildChatView(const ViewInputs& inputs) {
    ChatView view;
    const bool busy = inFlight(inputs.state);
    const bool empty = inputs.transcript == nullptr || inputs.transcript->empty();

    if (!inputs.gameFolderFound) {
        view.status = std::string(inputs.ck3FolderInDocuments ? text::kFolderNotFoundDocuments
                                                              : text::kFolderNotFound);
        view.statusTone = Tone::Warning;
    } else if (inputs.hasSnapshot && inputs.date.has_value()) {
        view.status = text::seenFrom(*inputs.date);
    } else if (empty) {
        // The first thing to do, in the middle of the window rather than in a corner.
        view.emptyState = std::string(text::kNothingReadYet);
    } else {
        view.status = std::string(text::kNothingReadYet);
    }

    if (!inputs.configured) {
        view.banners.push_back(Banner{inputs.notConfiguredMessage.empty()
                                          ? std::string(kDefaultNotConfigured)
                                          : inputs.notConfiguredMessage,
                                      Tone::Warning});
    }
    if (inputs.unsupportedFormat) {
        view.banners.push_back(Banner{std::string(text::kVersionMismatch), Tone::Warning});
    } else if (inputs.unreadable) {
        view.banners.push_back(Banner{std::string(text::kUnreadable), Tone::Warning});
    }
    if (inputs.hasSnapshot && inputs.probe) {
        view.banners.push_back(Banner{std::string(text::kProbe), Tone::Plain});
    }
    switch (inputs.notice) {
    case advisor::Notice::None:
        break;
    case advisor::Notice::RestartCk3:
        view.banners.push_back(Banner{std::string(text::kRestartCk3), Tone::Warning});
        break;
    case advisor::Notice::SilentSoft:
        view.banners.push_back(Banner{std::string(text::kSilentSoft), Tone::Warning});
        break;
    case advisor::Notice::Ck3NotRunning:
        view.banners.push_back(Banner{std::string(text::kCk3NotRunning), Tone::Warning});
        break;
    }

    if (inputs.transcript != nullptr) {
        const std::vector<TranscriptEntry>& transcript = *inputs.transcript;
        const std::optional<std::size_t> retryable = lastAnswer(transcript);
        for (std::size_t i = 0; i < transcript.size(); ++i) {
            const TranscriptEntry& entry = transcript[i];
            switch (entry.kind) {
            case EntryKind::User: {
                ViewItem item;
                item.kind = ItemKind::Question;
                item.entry = i;
                item.text = entry.text;
                view.items.push_back(std::move(item));
                break;
            }
            case EntryKind::Assistant: {
                const bool retryAllowed =
                    !busy && inputs.configured && retryable.has_value() && *retryable == i;
                ViewItem item = answerItem(entry, i, retryAllowed, inputs.registry);
                // The dot of "thinking...", and the one after the text while it arrives.
                if ((item.state == AnswerState::Thinking || item.state == AnswerState::Streaming) &&
                    busy) {
                    view.spinnerActive = true;
                }
                view.items.push_back(std::move(item));
                break;
            }
            case EntryKind::Divider: {
                ViewItem item;
                item.kind = ItemKind::Divider;
                item.entry = i;
                item.text = entry.text;
                view.items.push_back(std::move(item));
                break;
            }
            }
        }
    }

    AnswerLine reminder;
    reminder.runs.push_back(Run{std::string(text::kPauseReminderLead), true});
    reminder.runs.push_back(Run{std::string(text::kPauseReminderRest), false});
    view.pauseReminder.lines.push_back(std::move(reminder));
    view.inputHint = std::string(text::kInputHint);
    view.canSend = inputs.configured && !busy && !inputs.draftEmpty;
    view.canStop = busy;
    return view;
}

ViewInputs viewInputs(const AppController& controller, const WindowFacts& facts) {
    ViewInputs inputs;
    const advisor::ChatSession& session = controller.session();
    inputs.transcript = &session.transcript();
    inputs.state = session.state();
    inputs.configured = controller.configured();
    inputs.notConfiguredMessage = controller.notConfiguredMessage();
    const advisor::SnapshotStore& store = controller.snapshot();
    if (const bridge::Frame* frame = store.frame()) {
        inputs.hasSnapshot = true;
        inputs.date = frame->date;
        inputs.probe = frame->mode == 1;
    }
    inputs.unreadable = store.unreadable();
    inputs.unsupportedFormat = facts.unsupportedFormat;
    inputs.notice = controller.notice();
    inputs.gameFolderFound = facts.gameFolderFound;
    inputs.ck3FolderInDocuments = facts.ck3FolderInDocuments;
    inputs.draftEmpty = facts.draftEmpty;
    inputs.registry = controller.registry();
    return inputs;
}

}  // namespace curia::chatview
