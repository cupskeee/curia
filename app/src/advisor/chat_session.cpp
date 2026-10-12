// SPDX-License-Identifier: MIT
#include "advisor/chat_session.h"

#include <chrono>
#include <utility>

#include "advisor/chat_error_text.h"

namespace curia::advisor {

const char* toString(ChatState state) noexcept {
    switch (state) {
    case ChatState::Idle:
        return "Idle";
    case ChatState::Waiting:
        return "Waiting";
    case ChatState::Streaming:
        return "Streaming";
    case ChatState::Done:
        return "Done";
    case ChatState::Error:
        return "Error";
    case ChatState::Cancelled:
        return "Cancelled";
    }
    return "Unknown";
}

const char* toString(AskOutcome outcome) noexcept {
    switch (outcome) {
    case AskOutcome::Started:
        return "Started";
    case AskOutcome::EmptyText:
        return "EmptyText";
    case AskOutcome::Busy:
        return "Busy";
    case AskOutcome::NoProvider:
        return "NoProvider";
    case AskOutcome::NothingToRetry:
        return "NothingToRetry";
    }
    return "Unknown";
}

namespace {

constexpr std::string_view kMaxTokensStopReason = "max_tokens";

bool isWordChar(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_';
}

std::string trimmed(std::string_view text) {
    const std::string_view blanks = " \t\r\n";
    const std::size_t first = text.find_first_not_of(blanks);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(blanks);
    return std::string(text.substr(first, last - first + 1));
}

}  // namespace

ChatSession::ChatSession(std::shared_ptr<UiMailbox> mailbox, Options options)
    : mailbox_(std::move(mailbox)), options_(std::move(options)) {
    if (!options_.clock) {
        options_.clock = steadyNow;
    }
    askedAt_ = options_.clock();
}

ChatSession::~ChatSession() {
    if (inFlight()) {
        token_.cancel();
    }
}

const TranscriptEntry* ChatSession::currentAnswer() const {
    return hasAnswer_ ? &transcript_[answerIndex_] : nullptr;
}

std::string ChatSession::scrub(std::string_view text) const {
    return options_.registry != nullptr ? options_.registry->redact(text) : curia::redact(text);
}

void ChatSession::showAnswer(bool final) {
    // The final text also loses a key that the end of the stream cut in two (cancel, a dropped
    // connection, an error): redact() needs the whole key to know it.
    std::string shown =
        final ? (options_.registry != nullptr ? options_.registry->redactFinal(rawAnswer_)
                                              : curia::redactFinal(rawAnswer_))
              : scrub(rawAnswer_);
    if (!final) {
        // The start of a key that is still arriving looks like a word: wait for its end.
        std::size_t end = shown.size();
        while (end > 0 && isWordChar(shown[end - 1])) {
            --end;
        }
        shown.resize(end);
    }
    transcript_[answerIndex_].text = std::move(shown);
}

void ChatSession::setState(ChatState state) {
    if (state_ == state) {
        return;
    }
    state_ = state;
    if (observer_ != nullptr) {
        observer_->onChatState(state);
    }
}

void ChatSession::notify(std::size_t index) {
    if (observer_ != nullptr) {
        observer_->onTranscriptChanged(index);
    }
}

long long ChatSession::msSinceAsk() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(options_.clock() - askedAt_)
        .count();
}

providers::StreamHandlers ChatSession::makeHandlers(std::uint64_t generation) const {
    // The handlers run on the provider's thread. They hold the mailbox (shared, so it outlives a
    // late callback) and the generation, and nothing else.
    const std::shared_ptr<UiMailbox> box = mailbox_;
    providers::StreamHandlers handlers;
    handlers.onToken = [box, generation](providers::TokenKind kind, std::string_view text) {
        box->push(UiMessage{ProviderToken{generation, kind, std::string(text)}});
    };
    handlers.onStatus = [box, generation](providers::StreamStatus status) {
        box->push(UiMessage{ProviderStatus{generation, status}});
    };
    // The end of a request must never be lost to the queue's limit: forced.
    handlers.onDone = [box, generation](const providers::Usage& usage) {
        box->push(UiMessage{ProviderDone{generation, usage}}, true);
    };
    handlers.onError = [box, generation](const providers::ChatError& error) {
        box->push(UiMessage{ProviderError{generation, error}}, true);
    };
    return handlers;
}

AskOutcome ChatSession::ask(std::string_view text, const AskContext& context) {
    if (inFlight()) {
        return AskOutcome::Busy;
    }
    if (provider_ == nullptr) {
        return AskOutcome::NoProvider;
    }
    const std::string question = trimmed(scrub(text));
    if (question.empty()) {
        return AskOutcome::EmptyText;
    }
    TranscriptEntry user;
    user.kind = EntryKind::User;
    user.text = question;
    TranscriptEntry answer;
    answer.kind = EntryKind::Assistant;
    transcript_.push_back(std::move(user));
    transcript_.push_back(std::move(answer));
    const std::size_t questionIndex = transcript_.size() - 2;
    const AskOutcome outcome = start(questionIndex, transcript_.size() - 1, context);
    if (outcome != AskOutcome::Started) {
        transcript_.resize(transcript_.size() - 2);
    }
    return outcome;
}

AskOutcome ChatSession::retry(const AskContext& context) {
    if (inFlight()) {
        return AskOutcome::Busy;
    }
    if (provider_ == nullptr) {
        return AskOutcome::NoProvider;
    }
    std::size_t questionIndex = transcript_.size();
    for (std::size_t i = transcript_.size(); i > 0; --i) {
        if (transcript_[i - 1].kind == EntryKind::User) {
            questionIndex = i - 1;
            break;
        }
    }
    if (questionIndex == transcript_.size()) {
        return AskOutcome::NothingToRetry;
    }
    std::size_t answerIndex = questionIndex + 1;
    while (answerIndex < transcript_.size() &&
           transcript_[answerIndex].kind == EntryKind::Divider) {
        ++answerIndex;
    }
    if (answerIndex >= transcript_.size() ||
        transcript_[answerIndex].kind != EntryKind::Assistant) {
        return AskOutcome::NothingToRetry;
    }
    return start(questionIndex, answerIndex, context);
}

AskOutcome ChatSession::start(std::size_t questionIndex, std::size_t answerIndex,
                              const AskContext& context) {
    PromptInput input;
    input.settings = context.settings;
    input.snapshotText = context.snapshotText;
    input.transcript = &transcript_;
    input.questionIndex = questionIndex;
    input.freshness = context.freshness;
    const std::optional<providers::ChatRequest> request = buildChatRequest(input);
    if (!request.has_value()) {
        return AskOutcome::EmptyText;
    }

    ++generation_;
    answerIndex_ = answerIndex;
    hasAnswer_ = true;
    thinking_ = false;
    rawAnswer_.clear();
    askedAt_ = options_.clock();
    token_ = providers::CancelToken();
    TranscriptEntry& answer = transcript_[answerIndex];
    answer = TranscriptEntry{};
    answer.kind = EntryKind::Assistant;

    lastRequest_ = RequestInfo{};
    lastRequest_.generation = generation_;
    lastRequest_.personaBytes = request->systemPersona.size();
    lastRequest_.snapshotBytes = request->snapshotText.size();
    lastRequest_.conversationMessages = request->conversation.size();
    for (const providers::Message& message : request->conversation) {
        lastRequest_.conversationBytes += message.text.size();
    }

    setState(ChatState::Waiting);
    notify(questionIndex);
    notify(answerIndex);
    // Last: a provider may answer at once, on this thread; its handlers only post to the mailbox.
    provider_->streamChat(*request, makeHandlers(generation_), token_);
    return AskOutcome::Started;
}

bool ChatSession::cancel() {
    if (!inFlight()) {
        return false;
    }
    // Move the generation on first: whatever the stopped request still delivers is ignored.
    ++generation_;
    token_.cancel();
    thinking_ = false;
    showAnswer(true);
    TranscriptEntry& answer = transcript_[answerIndex_];
    answer.end = AnswerEnd::Stopped;
    answer.totalMs = msSinceAsk();
    setState(ChatState::Cancelled);
    notify(answerIndex_);
    return true;
}

void ChatSession::addDivider(std::string label) {
    if (!transcript_.empty() && transcript_.back().kind == EntryKind::Divider) {
        transcript_.back().text = std::move(label);
    } else {
        TranscriptEntry divider;
        divider.kind = EntryKind::Divider;
        divider.text = std::move(label);
        transcript_.push_back(std::move(divider));
    }
    notify(transcript_.size() - 1);
}

void ChatSession::onMessage(const UiMessage& message) {
    if (!inFlight()) {
        return;
    }
    if (const auto* token = std::get_if<ProviderToken>(&message)) {
        onToken(*token);
    } else if (const auto* status = std::get_if<ProviderStatus>(&message)) {
        onStatus(*status);
    } else if (const auto* done = std::get_if<ProviderDone>(&message)) {
        onDone(*done);
    } else if (const auto* error = std::get_if<ProviderError>(&message)) {
        onError(*error);
    }
}

void ChatSession::onToken(const ProviderToken& token) {
    if (token.generation != generation_) {
        return;
    }
    if (token.kind == providers::TokenKind::Thinking) {
        // The model's reasoning is not shown (showThinking is off); it only keeps the spinner on.
        thinking_ = true;
        notify(answerIndex_);
        return;
    }
    if (token.text.empty()) {
        return;
    }
    TranscriptEntry& answer = transcript_[answerIndex_];
    if (answer.firstTokenMs < 0) {
        answer.firstTokenMs = msSinceAsk();
    }
    thinking_ = false;
    rawAnswer_ += token.text;
    showAnswer(false);
    setState(ChatState::Streaming);
    notify(answerIndex_);
}

void ChatSession::onStatus(const ProviderStatus& status) {
    if (status.generation != generation_) {
        return;
    }
    thinking_ = status.status == providers::StreamStatus::ThinkingStarted;
    notify(answerIndex_);
}

void ChatSession::onDone(const ProviderDone& done) {
    if (done.generation != generation_) {
        return;
    }
    thinking_ = false;
    TranscriptEntry& answer = transcript_[answerIndex_];
    answer.usage = done.usage;
    answer.hitTokenLimit = done.usage.stopReason == kMaxTokensStopReason;
    answer.totalMs = msSinceAsk();
    if (rawAnswer_.empty()) {
        // No text at all: say so, instead of a blank answer that looks like a hang.
        answer.end = AnswerEnd::Failed;
        ErrorInfo info;
        info.code = providers::ChatError::Code::Other;
        info.summary = answer.hitTokenLimit
                           ? "The answer ran out of room before any text appeared. Try again, or "
                             "ask a shorter question."
                           : "The service sent an empty answer. Try again.";
        info.retryable = true;
        answer.error = std::move(info);
        setState(ChatState::Error);
        notify(answerIndex_);
        return;
    }
    showAnswer(true);
    answer.end = AnswerEnd::Complete;
    setState(ChatState::Done);
    notify(answerIndex_);
}

void ChatSession::onError(const ProviderError& error) {
    if (error.generation != generation_) {
        return;
    }
    thinking_ = false;
    TranscriptEntry& answer = transcript_[answerIndex_];
    // The provider's partial text is the whole text received; ours may lack the tokens that were
    // still in the mailbox.
    if (error.error.partialText.size() > rawAnswer_.size()) {
        rawAnswer_ = error.error.partialText;
    }
    showAnswer(true);
    answer.totalMs = msSinceAsk();
    if (error.error.code == providers::ChatError::Code::Cancelled) {
        answer.end = AnswerEnd::Stopped;
        setState(ChatState::Cancelled);
    } else {
        answer.end = AnswerEnd::Failed;
        answer.error = describeChatError(error.error, options_.registry);
        setState(ChatState::Error);
    }
    notify(answerIndex_);
}

}  // namespace curia::advisor
