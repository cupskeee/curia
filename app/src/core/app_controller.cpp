// SPDX-License-Identifier: MIT
#include "core/app_controller.h"

#include <initializer_list>
#include <utility>
#include <variant>
#include <vector>

#include "advisor/persona.h"
#include "advisor/snapshot_renderer.h"
#include "bridge/frame_json.h"  // toString(DamageReason)

namespace curia {

const char* toString(OverlaySource source) noexcept {
    switch (source) {
    case OverlaySource::GameClick:
        return "game_click";
    case OverlaySource::Hotkey:
        return "hotkey";
    case OverlaySource::Tray:
        return "tray";
    case OverlaySource::Refresh:
        return "refresh";
    }
    return "unknown";
}

namespace {

AppController::Options withClock(AppController::Options options) {
    if (!options.clock) {
        options.clock = steadyNow;
    }
    return options;
}

advisor::ChatSession::Options sessionOptions(const AppController::Options& options) {
    advisor::ChatSession::Options session;
    session.registry = options.registry;
    session.clock = options.clock;
    return session;
}

constexpr std::string_view kDefaultNotConfigured = "Curia is not set up to talk to a service yet.";

}  // namespace

AppController::AppController(Options options)
    : options_(withClock(std::move(options))), mailbox_(makeUiMailbox(options_.wake)),
      store_(options_.clock), monitor_(options_.clock, options_.silence),
      session_(mailbox_, sessionOptions(options_)), notConfigured_(kDefaultNotConfigured) {
    session_.setObserver(this);
}

AppController::~AppController() {
    session_.setObserver(nullptr);
}

void AppController::setObserver(AppObserver* observer) {
    observer_ = observer;
}

void AppController::log(AppLog::Level level, std::string_view event,
                        std::initializer_list<AppLog::Field> fields) const {
    if (options_.log != nullptr) {
        options_.log->log(level, event, fields);
    }
}

bridge::LogWatcher::Callbacks AppController::watcherCallbacks() {
    // The callbacks run on the watcher thread and hold the mailbox only.
    const std::shared_ptr<UiMailbox> box = mailbox_;
    bridge::LogWatcher::Callbacks callbacks;
    callbacks.onEvent = [box](bridge::Event&& event) {
        box->push(UiMessage{makeWatcherEvent(std::move(event))});
    };
    callbacks.onNotice = [box](bridge::TailerNotice notice) {
        box->push(UiMessage{LogNotice{notice}});
    };
    callbacks.onProgress = [box](const bridge::TailerStats& stats) {
        box->push(UiMessage{LogProgress{stats}});
    };
    return callbacks;
}

bool AppController::setProvider(std::shared_ptr<providers::Provider> provider, std::string model) {
    if (provider == nullptr || model.empty()) {
        return false;
    }
    session_.cancel();
    provider_ = std::move(provider);
    model_ = std::move(model);
    notConfigured_.clear();
    session_.setProvider(provider_.get());
    log(AppLog::Level::Info, "provider_set", {{"persona_version", advisor::kPersonaVersion}});
    return true;
}

void AppController::setNotConfigured(std::string message) {
    session_.cancel();
    session_.setProvider(nullptr);
    provider_.reset();
    model_.clear();
    notConfigured_ = message.empty() ? std::string(kDefaultNotConfigured) : std::move(message);
    log(AppLog::Level::Info, "not_configured");
}

std::size_t AppController::pump() {
    std::vector<UiMessage> messages = mailbox_->drain();
    const std::size_t count = messages.size();
    for (UiMessage& message : messages) {
        onMessage(std::move(message));
    }
    const std::uint64_t dropped = mailbox_->dropped();
    if (dropped > lastDropped_) {
        log(AppLog::Level::Warn, "mailbox_dropped", {{"total", dropped}});
        lastDropped_ = dropped;
    }
    return count;
}

void AppController::onMessage(UiMessage message) {
    if (const auto* watcher = std::get_if<WatcherEvent>(&message)) {
        if (watcher->event != nullptr) {
            handleWatcherEvent(*watcher->event);
        }
    } else if (const auto* progress = std::get_if<LogProgress>(&message)) {
        if (progress->stats.totalBytes > lastTotalBytes_) {
            monitor_.logGrew();
        }
        lastTotalBytes_ = progress->stats.totalBytes;
    } else if (const auto* notice = std::get_if<LogNotice>(&message)) {
        handleLogNotice(notice->notice);
    } else if (const auto* running = std::get_if<Ck3RunningChanged>(&message)) {
        monitor_.ck3Running(running->running);
    } else {
        // The provider's messages; the session ignores the ones of an old generation.
        session_.onMessage(message);
    }
    refreshNotice();
}

void AppController::handleLogNotice(bridge::TailerNotice notice) {
    if (notice == bridge::TailerNotice::Truncated) {
        // The game truncates its log at launch: it is alive and writing.
        lastTotalBytes_ = 0;
        monitor_.logGrew();
        log(AppLog::Level::Info, "log_truncated");
        return;
    }
    log(AppLog::Level::Info, "log_notice", {{"notice", bridge::toString(notice)}});
}

void AppController::handleWatcherEvent(const bridge::Event& event) {
    if (const auto* complete = std::get_if<bridge::FrameComplete>(&event)) {
        handleFrame(*complete);
    } else if (const auto* damaged = std::get_if<bridge::FrameDamaged>(&event)) {
        handleDamaged(*damaged);
    } else if (const auto* unsupported = std::get_if<bridge::UnsupportedFormat>(&event)) {
        handleUnsupported(*unsupported);
    } else if (const auto* diagnostic = std::get_if<bridge::Diagnostic>(&event)) {
        log(AppLog::Level::Debug, "log_diagnostic",
            {{"reason", bridge::toString(diagnostic->reason)}});
    }
}

SnapshotInfo AppController::makeInfo(const advisor::SnapshotStore::Update& update) const {
    SnapshotInfo info;
    info.sequence = store_.sequence();
    info.unreadable = store_.unreadable();
    info.first = update.first;
    info.dateWentBack = update.dateWentBack;
    if (const bridge::Frame* frame = store_.frame()) {
        info.date = frame->date;
        info.probe = frame->mode == 1;
    }
    return info;
}

void AppController::handleFrame(const bridge::FrameComplete& complete) {
    const std::int32_t snap = complete.frame.snap;
    const std::int32_t mode = complete.frame.mode;
    const std::int32_t records = complete.frame.recordCount;
    const bool shortened = complete.frame.textShortened;
    const advisor::SnapshotStore::Update update = store_.onFrameComplete(complete.frame);
    monitor_.frameArrived();
    if (!session_.transcript().empty()) {
        std::string label =
            "New snapshot, game date " + advisor::formatGameDate(store_.frame()->date);
        if (update.dateWentBack) {
            label += " (a save may have been loaded)";
        }
        session_.addDivider(std::move(label));
    }
    log(AppLog::Level::Info, "frame_complete",
        {{"snap", snap},
         {"mode", mode},
         {"records", records},
         {"text_shortened", shortened},
         {"snapshot_bytes", store_.text().size()}});
    const SnapshotInfo info = makeInfo(update);
    if (observer_ != nullptr) {
        observer_->onSnapshotChanged(info);
        observer_->onOverlayRequested(info.probe);
    }
}

void AppController::handleDamaged(const bridge::FrameDamaged& damaged) {
    store_.onExportUnreadable();
    monitor_.frameDamaged();
    log(AppLog::Level::Warn, "frame_damaged",
        {{"snap", damaged.snap}, {"reason", bridge::toString(damaged.reason)}});
    if (observer_ != nullptr) {
        observer_->onSnapshotChanged(makeInfo({}));
    }
}

void AppController::handleUnsupported(const bridge::UnsupportedFormat& unsupported) {
    store_.onExportUnreadable();
    monitor_.frameDamaged();
    log(AppLog::Level::Warn, "frame_unsupported", {{"snap", unsupported.snap}});
    if (observer_ != nullptr) {
        observer_->onSnapshotChanged(makeInfo({}));
        observer_->onUnsupportedFormat(unsupported.leadingFields);
    }
}

advisor::AskContext AppController::makeContext() const {
    const TimePoint now = options_.clock();
    advisor::AskContext context;
    context.settings.model = model_;
    context.settings.maxTokens = options_.maxTokens;
    context.snapshotText = store_.hasSnapshot() ? store_.text() : std::string();
    context.freshness = store_.freshness(now, monitor_.secondsSinceLogGrew(now).value_or(0));
    return context;
}

void AppController::logAskStarted(advisor::AskOutcome outcome) {
    if (outcome != advisor::AskOutcome::Started) {
        log(AppLog::Level::Info, "ask_refused", {{"outcome", advisor::toString(outcome)}});
        return;
    }
    const advisor::RequestInfo& info = session_.lastRequest();
    const TimePoint now = options_.clock();
    const std::optional<TimePoint> arrived = store_.arrivedAt();
    log(AppLog::Level::Info, "ask_started",
        {{"gen", info.generation},
         {"persona_version", advisor::kPersonaVersion},
         {"persona_bytes", info.personaBytes},
         {"snapshot_bytes", info.snapshotBytes},
         {"messages", info.conversationMessages},
         {"conversation_bytes", info.conversationBytes},
         {"snapshot_age_s", arrived.has_value() ? secondsBetween(*arrived, now) : 0},
         {"unreadable", store_.unreadable()}});
}

advisor::AskOutcome AppController::onUserAsk(std::string_view text) {
    const advisor::AskOutcome outcome = session_.ask(text, makeContext());
    logAskStarted(outcome);
    return outcome;
}

advisor::AskOutcome AppController::onRetry() {
    const advisor::AskOutcome outcome = session_.retry(makeContext());
    logAskStarted(outcome);
    return outcome;
}

void AppController::onStop() {
    session_.cancel();
}

void AppController::onOverlayOpened(OverlaySource source) {
    log(AppLog::Level::Info, "overlay_opened", {{"source", toString(source)}});
    if (source != OverlaySource::GameClick) {
        monitor_.overlayOpenedByUser();
    }
    refreshNotice();
}

void AppController::onTick(TimePoint now) {
    monitor_.tick(now);
    refreshNotice();
}

void AppController::onCk3Running(bool running) {
    monitor_.ck3Running(running);
    refreshNotice();
}

void AppController::refreshNotice() {
    const advisor::Notice current = monitor_.notice();
    if (current == lastNotice_) {
        return;
    }
    lastNotice_ = current;
    log(AppLog::Level::Info, "notice", {{"name", advisor::toString(current)}});
    if (observer_ != nullptr) {
        observer_->onNoticeChanged(current);
    }
}

void AppController::onChatState(advisor::ChatState state) {
    const advisor::TranscriptEntry* answer = session_.currentAnswer();
    // The request's own number, the one ask_started logged (the session's counter moves on at a
    // stop).
    const std::uint64_t generation = session_.lastRequest().generation;
    switch (state) {
    case advisor::ChatState::Streaming:
        if (answer != nullptr) {
            log(AppLog::Level::Info, "ask_first_token",
                {{"gen", generation}, {"first_token_ms", answer->firstTokenMs}});
        }
        break;
    case advisor::ChatState::Done:
        if (answer != nullptr) {
            log(AppLog::Level::Info, "ask_done",
                {{"gen", generation},
                 {"input", answer->usage.input},
                 {"output", answer->usage.output},
                 {"cache_read", answer->usage.cacheRead},
                 {"cache_write", answer->usage.cacheWrite},
                 {"max_tokens", answer->hitTokenLimit},
                 {"first_token_ms", answer->firstTokenMs},
                 {"total_ms", answer->totalMs},
                 {"answer_bytes", answer->text.size()}});
        }
        break;
    case advisor::ChatState::Error:
        if (answer != nullptr && answer->error.has_value()) {
            log(AppLog::Level::Warn, "ask_error",
                {{"gen", generation},
                 {"code", providers::toString(answer->error->code)},
                 {"status", answer->error->httpStatus},
                 {"retryable", answer->error->retryable},
                 {"total_ms", answer->totalMs},
                 {"answer_bytes", answer->text.size()}});
        }
        break;
    case advisor::ChatState::Cancelled:
        if (answer != nullptr) {
            log(AppLog::Level::Info, "ask_cancelled",
                {{"gen", generation},
                 {"total_ms", answer->totalMs},
                 {"answer_bytes", answer->text.size()}});
        }
        break;
    case advisor::ChatState::Idle:
    case advisor::ChatState::Waiting:
        break;
    }
    if (observer_ != nullptr) {
        observer_->onChatState(state);
    }
}

void AppController::onTranscriptChanged(std::size_t index) {
    if (observer_ != nullptr) {
        observer_->onTranscriptChanged(index);
    }
}

}  // namespace curia
