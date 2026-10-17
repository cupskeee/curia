// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "advisor/chat_session.h"
#include "advisor/log_silence_monitor.h"
#include "advisor/snapshot_store.h"
#include "bridge/frame.h"
#include "bridge/log_watcher.h"
#include "core/app_log.h"
#include "core/clock.h"
#include "core/redact.h"
#include "core/ui_message.h"
#include "providers/provider.h"

namespace curia {

// Where the overlay was opened from. The in-game click is not a request by the player to refresh;
// the other three are, and arm the log-silence detector when the snapshot is old.
enum class OverlaySource { GameClick, Hotkey, Tray, Refresh };

const char* toString(OverlaySource source) noexcept;

// What the chat view shows while no provider is set up, unless setNotConfigured() gave a message.
inline constexpr std::string_view kDefaultNotConfigured =
    "Curia is not set up to talk to a service yet.";

// The divider row added to a conversation when a newer reading of the realm arrives: "Updated
// from the game: 3 October 1071", plus " (a save may have been loaded)" when the game date went
// back. Plain words (advisor/plain_words.h).
std::string dividerLabel(const bridge::Date& date, bool dateWentBack);

// What the UI needs to know about the snapshot after a frame event.
struct SnapshotInfo {
    // Complete frames received so far.
    std::uint64_t sequence = 0;
    // The export is currently unreadable (a damaged frame since the last good one).
    bool unreadable = false;
    // This frame came from the diagnostic probe, not the Advisor button.
    bool probe = false;
    bool first = false;
    bool dateWentBack = false;
    std::optional<bridge::Date> date;
};

// Told, on the UI thread, what changed. Every method has an empty default.
class AppObserver : public advisor::ChatObserver {
public:
    // A frame was stored, or the export became unreadable.
    virtual void onSnapshotChanged(const SnapshotInfo& /*info*/) {}
    // A Complete frame arrived: the player clicked Advisor in the game, so the overlay should open.
    virtual void onOverlayRequested(bool /*probeExport*/) {}
    virtual void onNoticeChanged(advisor::Notice /*notice*/) {}
    // The mod's frames are in a layout this app does not read (version mismatch). `leadingFields`
    // is the cleaned start of the BEGIN record (at most 64 bytes).
    virtual void onUnsupportedFormat(std::string_view /*leadingFields*/) {}
};

// Wires the pieces of the advisor behind one object for the UI thread: the log watcher's events,
// the snapshot store, the log-silence monitor, the chat session and the provider.
//
// Threads: the watcher and the provider's net thread never call it. Their callbacks (see
// watcherCallbacks() and ChatSession) push UiMessages into the mailbox, whose wake-up callback
// (Options::wake) tells the UI thread to call pump(). Every other method belongs to the UI thread.
//
// Not configured: setNotConfigured() stores the message to show (built by describeNotConfigured()
// from the DevConfig, which names the missing variables and never a value). onUserAsk() then
// returns AskOutcome::NoProvider and starts nothing.
//
// The app log gets numbers and short words only: sizes, counts, timings, usage, state names. Never
// a prompt, an answer, a name or an error body.
class AppController final : private advisor::ChatObserver {
public:
    struct Options {
        // Empty = steadyNow.
        ClockFn clock;
        // Not owned, may be null; must outlive the controller.
        AppLog* log = nullptr;
        // The secrets to scrub from answers and questions; null = the process-wide registry.
        const SecretRegistry* registry = nullptr;
        // Called (on a producer's thread) when the mailbox goes from empty to non-empty.
        UiMailbox::Wake wake;
        advisor::LogSilenceConfig silence;
        // What every request asks for: the answer limit, and the effort and thinking settings
        // (Default sends nothing; decision D22).
        int maxTokens = 4096;
        providers::Effort effort = providers::Effort::Default;
        providers::Thinking thinking = providers::Thinking::Default;
        // Ask the provider to report the model's reasoning as it happens (the session shows only
        // that the model is thinking, never the reasoning text).
        bool showThinking = false;
    };

    explicit AppController(Options options);
    ~AppController();
    AppController(const AppController&) = delete;
    AppController& operator=(const AppController&) = delete;

    void setObserver(AppObserver* observer);

    // Callbacks for bridge::LogWatcher that post into the mailbox.
    bridge::LogWatcher::Callbacks watcherCallbacks();
    std::shared_ptr<UiMailbox> mailbox() const { return mailbox_; }

    // --- configuration (UI thread) ---------------------------------------------------------
    // Uses `provider` with `model`; an empty model or a null provider is refused (false). A request
    // in flight is stopped first.
    bool setProvider(std::shared_ptr<providers::Provider> provider, std::string model);
    // Drops the provider and remembers why (the text for the chat view).
    void setNotConfigured(std::string message);
    bool configured() const { return provider_ != nullptr; }
    const std::string& notConfiguredMessage() const { return notConfigured_; }

    // --- the UI thread's inputs ------------------------------------------------------------
    // Drains the mailbox and handles every message; returns how many. Call it when the wake-up
    // event arrives.
    std::size_t pump();
    void onMessage(UiMessage message);

    advisor::AskOutcome onUserAsk(std::string_view text);
    advisor::AskOutcome onRetry();
    void onStop();
    void onOverlayOpened(OverlaySource source);
    void onTick(TimePoint now);
    void onCk3Running(bool running);

    // --- state for drawing -----------------------------------------------------------------
    const advisor::ChatSession& session() const { return session_; }
    const advisor::SnapshotStore& snapshot() const { return store_; }
    advisor::Notice notice() const { return monitor_.notice(); }
    const advisor::LogSilenceMonitor& monitor() const { return monitor_; }

private:
    // ChatObserver (the session's callbacks)
    void onChatState(advisor::ChatState state) override;
    void onTranscriptChanged(std::size_t index) override;

    advisor::AskContext makeContext() const;
    void logAskStarted(advisor::AskOutcome outcome);
    void handleWatcherEvent(const bridge::Event& event);
    void handleFrame(const bridge::FrameComplete& complete);
    void handleDamaged(const bridge::FrameDamaged& damaged);
    void handleUnsupported(const bridge::UnsupportedFormat& unsupported);
    void handleLogNotice(bridge::TailerNotice notice);
    void refreshNotice();
    SnapshotInfo makeInfo(const advisor::SnapshotStore::Update& update) const;
    void log(AppLog::Level level, std::string_view event,
             std::initializer_list<AppLog::Field> fields = {}) const;

    Options options_;
    std::shared_ptr<UiMailbox> mailbox_;
    advisor::SnapshotStore store_;
    advisor::LogSilenceMonitor monitor_;
    // Declared after the mailbox and the store: it holds the mailbox.
    advisor::ChatSession session_;
    std::shared_ptr<providers::Provider> provider_;
    std::string model_;
    std::string notConfigured_;
    AppObserver* observer_ = nullptr;
    advisor::Notice lastNotice_ = advisor::Notice::None;
    std::uint64_t lastTotalBytes_ = 0;
    std::uint64_t lastDropped_ = 0;
};

}  // namespace curia
