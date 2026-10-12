// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>

#include "bridge/events.h"
#include "bridge/log_tailer.h"
#include "core/mailbox.h"
#include "providers/chat_types.h"

namespace curia {

// Everything that crosses from another thread to the UI thread. Producers only build one of these
// and push it; they never touch advisor state. The UI thread drains the mailbox and hands each
// message to AppController::onMessage().

// Provider callbacks (net thread). `generation` names the request they belong to; a message of an
// older generation (cancelled or superseded) is ignored by the session.
struct ProviderToken {
    std::uint64_t generation = 0;
    providers::TokenKind kind = providers::TokenKind::Text;
    std::string text;
};
struct ProviderStatus {
    std::uint64_t generation = 0;
    providers::StreamStatus status = providers::StreamStatus::TextStarted;
};
struct ProviderDone {
    std::uint64_t generation = 0;
    providers::Usage usage;
};
struct ProviderError {
    std::uint64_t generation = 0;
    providers::ChatError error;
};

// Log watcher callbacks (watcher thread).
// The event is shared and immutable: copying a message copies a pointer, not a frame (and the
// message variant never has to move a variant that holds a variant).
struct WatcherEvent {
    std::shared_ptr<const bridge::Event> event;
};

inline WatcherEvent makeWatcherEvent(bridge::Event event) {
    return WatcherEvent{std::make_shared<const bridge::Event>(std::move(event))};
}
struct LogProgress {
    bridge::TailerStats stats;
};
struct LogNotice {
    bridge::TailerNotice notice = bridge::TailerNotice::Attached;
};

// From a platform poller (is CK3 running?).
struct Ck3RunningChanged {
    bool running = false;
};

using UiMessage = std::variant<ProviderToken, ProviderStatus, ProviderDone, ProviderError,
                               WatcherEvent, LogProgress, LogNotice, Ck3RunningChanged>;

using UiMailbox = Mailbox<UiMessage>;

// One text token message absorbs the next one of the same request and kind, up to this size, so
// that a stalled consumer holds a few large messages instead of thousands of small ones.
inline constexpr std::size_t kMaxCoalescedTokenBytes = 64 * 1024;

// The merge function of the UI mailbox: joins consecutive ProviderToken messages of one generation
// and kind (text is appended in order, nothing is dropped).
bool mergeUiMessages(UiMessage& pending, UiMessage& incoming);

// The mailbox the app uses: token coalescing on, `wake` called on the empty-to-non-empty
// transition.
std::shared_ptr<UiMailbox> makeUiMailbox(UiMailbox::Wake wake,
                                         std::size_t maxMessages = UiMailbox::kDefaultMaxMessages);

}  // namespace curia
