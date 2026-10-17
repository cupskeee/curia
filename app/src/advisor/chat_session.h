// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/freshness.h"
#include "advisor/prompt_builder.h"
#include "advisor/transcript.h"
#include "core/clock.h"
#include "core/redact.h"
#include "core/ui_message.h"
#include "providers/cancel_token.h"
#include "providers/provider.h"

namespace curia::advisor {

enum class ChatState { Idle, Waiting, Streaming, Done, Error, Cancelled };

const char* toString(ChatState state) noexcept;

// What ask() and retry() did.
enum class AskOutcome {
    Started,
    // The question is blank.
    EmptyText,
    // A request is already in flight (Stop it first).
    Busy,
    // No provider is set (not configured).
    NoProvider,
    // retry() with no earlier question.
    NothingToRetry,
};

const char* toString(AskOutcome outcome) noexcept;

// The error summaries of an answer that ended with no text at all (plain words, plain_words.h).
inline constexpr std::string_view kEmptyAnswerText = "The service sent an empty answer. Try again.";
inline constexpr std::string_view kNoRoomForAnswerText =
    "The answer ran out of room before any text appeared. Try again, or ask a shorter question.";

// Told, on the UI thread, when the session changes. Every method has an empty default.
class ChatObserver {
public:
    virtual ~ChatObserver() = default;
    // The state changed (one call per change; each ask ends in exactly one terminal state).
    virtual void onChatState(ChatState /*state*/) {}
    // The transcript entry at `index` was added or changed (text appended, answer ended, divider).
    virtual void onTranscriptChanged(std::size_t /*index*/) {}
};

// Everything a request needs that the session does not own.
struct AskContext {
    PromptSettings settings;
    // The rendered snapshot block (SnapshotStore::text()).
    std::string snapshotText;
    Freshness freshness;
};

// Sizes of the last request, for the app log (numbers only, never text).
struct RequestInfo {
    std::uint64_t generation = 0;
    std::size_t personaBytes = 0;
    std::size_t snapshotBytes = 0;
    std::size_t conversationMessages = 0;
    std::size_t conversationBytes = 0;
};

// The chat: the transcript, at most one request in flight, and the rules that keep late or
// duplicate callbacks from corrupting either.
//
// Threads: every method is called from the UI thread. The provider's callbacks run on its own
// thread and do NOT touch the session: they only post a UiMessage (tagged with the request's
// generation) into the mailbox. The UI thread drains the mailbox and passes each message to
// onMessage(). Each request has a generation number; cancel() and the next ask() move it on, and
// onMessage() ignores every message of another generation, so a late token or a second terminal
// callback of a stopped or superseded request changes nothing.
//
// Terminal states: an ask ends in exactly one of Done, Error and Cancelled, announced once. After
// that the request's generation is closed.
//   * cancel() keeps the text received so far, marks the answer Stopped and moves to Cancelled;
//   * retry() answers the last question again in place (the old answer is replaced);
//   * an answer that ends at the token limit is a normal Done with hitTokenLimit set;
//   * an error becomes a plain-language ErrorInfo (chat_error_text.h); partial text is kept.
// The answer text is kept raw in a private buffer (never in the transcript) and the transcript
// shows redact(whole buffer), so a key split over any number of tokens is found once it is
// complete. While the answer streams, the last word (the trailing run of letters, digits, '-'
// and '_') is held back from the transcript, so that the start of a key that is still arriving is
// never shown; it appears with the next blank or when the answer ends. The question is redacted
// before it is stored or sent. An answer that ends with no text at all is an error (retryable),
// not a blank answer.
class ChatSession {
public:
    struct Options {
        // The secrets to scrub; null = the process-wide registry.
        const SecretRegistry* registry = nullptr;
        // Empty = steadyNow.
        ClockFn clock;
    };

    ChatSession(std::shared_ptr<UiMailbox> mailbox, Options options);
    ~ChatSession();
    ChatSession(const ChatSession&) = delete;
    ChatSession& operator=(const ChatSession&) = delete;

    // The provider to use, or null (not configured). Not owned. Do not change it while a request
    // is in flight (cancel() first).
    void setProvider(providers::Provider* provider) { provider_ = provider; }
    void setObserver(ChatObserver* observer) { observer_ = observer; }

    AskOutcome ask(std::string_view text, const AskContext& context);
    AskOutcome retry(const AskContext& context);
    // Stops the request in flight; false if nothing is in flight.
    bool cancel();

    // Adds a divider row (a new snapshot); a divider directly after a divider replaces it.
    void addDivider(std::string label);

    // Feeds one message from the mailbox. Messages that are not for the request in flight are
    // ignored.
    void onMessage(const UiMessage& message);

    ChatState state() const noexcept { return state_; }
    bool inFlight() const noexcept {
        return state_ == ChatState::Waiting || state_ == ChatState::Streaming;
    }
    // The thinking phase of the model is on (a spinner is warranted).
    bool thinking() const noexcept { return thinking_; }
    std::uint64_t generation() const noexcept { return generation_; }
    const std::vector<TranscriptEntry>& transcript() const noexcept { return transcript_; }
    // The assistant entry of the current or last request; null before the first ask.
    const TranscriptEntry* currentAnswer() const;
    const RequestInfo& lastRequest() const noexcept { return lastRequest_; }

private:
    AskOutcome start(std::size_t questionIndex, std::size_t answerIndex, const AskContext& context);
    providers::StreamHandlers makeHandlers(std::uint64_t generation) const;
    std::string scrub(std::string_view text) const;
    // Sets the transcript text of the current answer from the raw buffer; `final` shows the last
    // word too.
    void showAnswer(bool final);
    void setState(ChatState state);
    void notify(std::size_t index);
    long long msSinceAsk() const;

    void onToken(const ProviderToken& token);
    void onStatus(const ProviderStatus& status);
    // The model is thinking: the spinner is on, and the first time before any text, the time.
    void noteThinking();
    void onDone(const ProviderDone& done);
    void onError(const ProviderError& error);

    std::shared_ptr<UiMailbox> mailbox_;
    Options options_;
    providers::Provider* provider_ = nullptr;
    ChatObserver* observer_ = nullptr;

    std::vector<TranscriptEntry> transcript_;
    ChatState state_ = ChatState::Idle;
    std::uint64_t generation_ = 0;
    providers::CancelToken token_;
    std::size_t answerIndex_ = 0;
    bool hasAnswer_ = false;
    bool thinking_ = false;
    // The text received for the current answer, before redaction. Never copied to the transcript.
    std::string rawAnswer_;
    TimePoint askedAt_;
    RequestInfo lastRequest_;
};

}  // namespace curia::advisor
