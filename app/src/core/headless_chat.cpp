// SPDX-License-Identifier: MIT
#include "core/headless_chat.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <istream>
#include <mutex>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include <nlohmann/json.hpp>

#include "advisor/chat_error_text.h"
#include "advisor/chat_session.h"
#include "advisor/log_silence_monitor.h"
#include "advisor/persona.h"
#include "advisor/snapshot_renderer.h"
#include "bridge/events.h"
#include "bridge/log_tailer.h"
#include "bridge/log_watcher.h"
#include "core/app_controller.h"
#include "core/check_api_report.h"
#include "core/headless_commands.h"
#include "core/json_lines.h"
#include "core/transcript_writer.h"

namespace curia {

namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

constexpr std::size_t kMaxFatalBytes = 300;

// What the input thread, the producers' wake-up and the main loop share. Held by shared_ptr: a
// reader that is still blocked when the run ends, and a late wake-up from the provider's thread,
// must find it alive.
struct Signal {
    std::mutex mutex;
    std::condition_variable cv;
    bool wake = false;
    bool input = false;
    std::deque<std::string> lines;
    bool eof = false;
    bool readerDone = false;

    void notifyWake() {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            wake = true;
        }
        cv.notify_all();
    }
};

// Reads one line without letting an endless line fill the memory: what is beyond the limit is
// dropped and the line is reported as overlong. Returns false at the end of the input.
bool readLine(std::istream& in, std::string& line, bool& overlong) {
    line.clear();
    overlong = false;
    bool any = false;
    for (;;) {
        const int ch = in.get();
        if (ch == std::istream::traits_type::eof()) {
            return any;
        }
        any = true;
        if (ch == '\n') {
            return true;
        }
        if (line.size() > kMaxCommandLineBytes) {
            overlong = true;
        } else {
            line.push_back(static_cast<char>(ch));
        }
    }
}

void readerMain(const std::shared_ptr<Signal>& signal, std::istream& in) {
    std::string line;
    bool overlong = false;
    while (readLine(in, line, overlong)) {
        {
            const std::lock_guard<std::mutex> lock(signal->mutex);
            // An overlong line is queued as one that the parser refuses.
            signal->lines.push_back(overlong ? std::string(kMaxCommandLineBytes + 2, 'x') : line);
            signal->input = true;
        }
        signal->cv.notify_all();
    }
    {
        const std::lock_guard<std::mutex> lock(signal->mutex);
        signal->eof = true;
        signal->readerDone = true;
        signal->input = true;
    }
    signal->cv.notify_all();
}

std::string noticeKind(advisor::Notice notice) {
    switch (notice) {
    case advisor::Notice::None:
        return "none";
    case advisor::Notice::RestartCk3:
        return "restart_ck3";
    case advisor::Notice::SilentSoft:
        return "silent_soft";
    case advisor::Notice::Ck3NotRunning:
        return "ck3_not_running";
    }
    return "none";
}

json nullableMs(long long value) {
    return value < 0 ? json(nullptr) : json(value);
}

class Runner final : public AppObserver {
public:
    Runner(HeadlessChatConfig config, std::istream& in, std::ostream& out, std::ostream& err)
        : config_(std::move(config)), in_(in), err_(err), writer_(out, config_.registry),
          signal_(std::make_shared<Signal>()) {}

    ~Runner() override {
        if (watcher_ != nullptr) {
            watcher_->stop();
        }
    }

    Runner(const Runner&) = delete;
    Runner& operator=(const Runner&) = delete;

    HeadlessChatResult run() {
        HeadlessChatResult result;
        try {
            result.exitCode = runInner();
        } catch (const std::exception& error) {
            result.exitCode =
                fatal("internal_error", describeFatalException(error.what(), config_.registry),
                      kHeadlessExitInternal);
        } catch (...) {
            result.exitCode =
                fatal("internal_error", "internal error: unknown exception", kHeadlessExitInternal);
        }
        if (watcher_ != nullptr) {
            watcher_->stop();
        }
        result.inputStillBlocked = finishReader();
        return result;
    }

private:
    // ---- setup ----------------------------------------------------------------------------

    int fatal(const std::string& code, const std::string& message, int exitCode) {
        writer_.write({{"event", "fatal"}, {"code", code}, {"message", message}});
        err_ << "curia: " << scrubbed(message) << '\n' << std::flush;
        return exitCode;
    }

    [[nodiscard]] std::string scrubbed(const std::string& text) const {
        return config_.registry != nullptr ? config_.registry->redact(text) : redact(text);
    }

    int runInner() {
        const CliOptions& options = config_.options;
        if (options.hasTestOnlyOptions() && !testOnlyOptionsAllowed()) {
            return fatal("setup", "this build does not accept the test-only options",
                         kHeadlessExitSetup);
        }
        for (const std::string& warning : config_.warnings) {
            err_ << "curia: note: " << warning << '\n';
        }
        err_ << std::flush;
        if (!config_.missing.empty() || config_.provider == nullptr || config_.model.empty()) {
            json missing = json::array();
            for (const std::string& name : config_.missing) {
                missing.push_back(name);
            }
            writer_.write({{"event", "not_configured"}, {"missing", missing}});
            for (const std::string& line : adviceForMissing(config_.missing)) {
                err_ << "curia: " << line << '\n';
            }
            err_ << std::flush;
            return kHeadlessExitSetup;
        }
        if (config_.logContent && !prepareTranscripts()) {
            return kHeadlessExitSetup;
        }
        std::error_code ec;
        std::uint64_t initialSize = 0;
        const bool exists = std::filesystem::exists(options.logPath, ec);
        if (exists && std::filesystem::is_directory(options.logPath, ec)) {
            return fatal("setup", "--log names a folder, not the game's debug.log file",
                         kHeadlessExitSetup);
        }
        if (exists) {
            const std::uintmax_t size = std::filesystem::file_size(options.logPath, ec);
            initialSize = ec ? 0 : static_cast<std::uint64_t>(size);
        } else if (options.fromStart) {
            return fatal("setup",
                         "the log file does not exist, so there is nothing to read from "
                         "its start",
                         kHeadlessExitSetup);
        }
        initialSize_ = initialSize;

        startController();
        startWatcher();
        startReader();
        return loop();
    }

    bool prepareTranscripts() {
        const platform::GetEnv getenv = config_.getenv ? config_.getenv : platform::getEnv;
        const TranscriptDir dir = resolveTranscriptDir(config_.transcriptDir, getenv);
        if (!dir.ok) {
            fatal("transcript_refused", dir.refusal, kHeadlessExitSetup);
            return false;
        }
        std::string error;
        if (!prepareTranscriptDir(dir.path, error)) {
            fatal("transcript_refused", error, kHeadlessExitSetup);
            return false;
        }
        transcriptDir_ = dir.path;
        return true;
    }

    void startController() {
        const CliOptions& options = config_.options;
        AppController::Options controllerOptions;
        controllerOptions.log = config_.log;
        controllerOptions.registry = config_.registry;
        const std::shared_ptr<Signal> signal = signal_;
        controllerOptions.wake = [signal] { signal->notifyWake(); };
        controllerOptions.maxTokens = options.maxTokens > 0 ? options.maxTokens : 4096;
        controllerOptions.effort = options.effort;
        controllerOptions.thinking = options.thinking;
        controllerOptions.showThinking = options.showThinking;
        if (options.armedWaitMs.has_value()) {
            controllerOptions.silence.armWait = std::chrono::milliseconds(*options.armedWaitMs);
        }
        if (options.armSnapshotAgeMs.has_value()) {
            controllerOptions.silence.armSnapshotAge =
                std::chrono::milliseconds(*options.armSnapshotAgeMs);
        }
        if (options.passiveSilenceMs.has_value()) {
            controllerOptions.silence.passiveSilence =
                std::chrono::milliseconds(*options.passiveSilenceMs);
        }
        controller_ = std::make_unique<AppController>(std::move(controllerOptions));
        controller_->setObserver(this);
        controller_->setProvider(config_.provider, config_.model);
    }

    void startWatcher() {
        const CliOptions& options = config_.options;
        bridge::LogWatcher::Callbacks callbacks = controller_->watcherCallbacks();
        const auto forwardEvent = callbacks.onEvent;
        const std::optional<int> frameFilter = options.frameSnap;
        callbacks.onEvent = [this, forwardEvent, frameFilter](bridge::Event&& event) {
            std::optional<std::int32_t> snap;
            if (const auto* complete = std::get_if<bridge::FrameComplete>(&event)) {
                ++framesSeen_;
                snap = complete->frame.snap;
            } else if (const auto* damaged = std::get_if<bridge::FrameDamaged>(&event)) {
                snap = damaged->snap;
            } else if (const auto* unsupported = std::get_if<bridge::UnsupportedFormat>(&event)) {
                snap = unsupported->snap;
            }
            if (frameFilter.has_value() && snap.has_value() && *snap != *frameFilter) {
                return;  // not the snapshot that was asked for: neither it nor its damage counts
            }
            forwardEvent(std::move(event));
        };
        const auto forwardProgress = callbacks.onProgress;
        callbacks.onProgress = [this, forwardProgress](const bridge::TailerStats& stats) {
            forwardProgress(stats);
            progressOffset_ = stats.offset;
            progressSeen_ = true;
            signal_->notifyWake();
        };
        bridge::LogTailer::Options tailerOptions;
        tailerOptions.fromStart = options.fromStart;
        auto tailer = std::make_unique<bridge::LogTailer>(
            std::make_unique<bridge::FileSource>(options.logPath), tailerOptions);
        watcher_ = std::make_unique<bridge::LogWatcher>(std::move(tailer), std::move(callbacks),
                                                        config_.pollInterval);
        watcher_->start();
    }

    void startReader() {
        const std::shared_ptr<Signal> signal = signal_;
        std::istream& in = in_;
        reader_ = std::thread([signal, &in] { readerMain(signal, in); });
    }

    // Joins the reader if it has finished; otherwise (a read blocked on the process's stdin)
    // leaves it behind. Returns whether it was left behind.
    bool finishReader() {
        if (!reader_.joinable()) {
            return false;
        }
        bool done = false;
        {
            std::unique_lock<std::mutex> lock(signal_->mutex);
            // A finished or finishing reader (the input was a string, or ended) is waited for
            // briefly; a blocked one is not.
            done = signal_->cv.wait_for(lock, std::chrono::milliseconds(200),
                                        [&] { return signal_->readerDone; });
        }
        if (done) {
            reader_.join();
            return false;
        }
        if (config_.inputIsProcessStdin) {
            reader_.detach();
            return true;
        }
        reader_.join();  // an input that is not stdin always ends
        return false;
    }

    // ---- the loop --------------------------------------------------------------------------

    int loop() {
        const CliOptions& options = config_.options;
        const int timeoutSeconds =
            options.timeoutSeconds > 0 ? options.timeoutSeconds : kDefaultHeadlessTimeoutSeconds;
        const auto deadline = Clock::now() + std::chrono::seconds(timeoutSeconds);
        const std::chrono::milliseconds tick(options.tickMs.value_or(kDefaultHeadlessTickMs));

        while (true) {
            {
                std::unique_lock<std::mutex> lock(signal_->mutex);
                signal_->cv.wait_for(lock, tick, [&] { return signal_->wake || signal_->input; });
                signal_->wake = false;
                signal_->input = false;
                while (!signal_->lines.empty()) {
                    pending_.push_back(std::move(signal_->lines.front()));
                    signal_->lines.pop_front();
                }
                inputEnded_ = signal_->eof;
            }
            const bool caughtUp = !ready_ && progressSeen_.load() &&
                                  (!options.fromStart || progressOffset_.load() >= initialSize_);
            controller_->pump();
            if (caughtUp) {
                const int code = becomeReady();
                if (code != 0) {
                    return code;
                }
            }
            controller_->onTick(Clock::now());
            if (ready_) {
                processCommands();
            }
            if (quit_) {
                controller_->onStop();
                controller_->pump();
                return exitCode();
            }
            if (ready_ && inputEnded_ && pending_.empty() && !controller_->session().inFlight()) {
                return exitCode();
            }
            if (Clock::now() >= deadline) {
                controller_->onStop();
                controller_->pump();
                return fatal("timeout", "the time limit ran out", kHeadlessExitTimeout);
            }
        }
    }

    [[nodiscard]] int exitCode() const {
        return anyFailed_ ? kHeadlessExitFailed : kHeadlessExitOk;
    }

    int becomeReady() {
        ready_ = true;
        const AppController& controller = *controller_;
        if (config_.options.frameSnap.has_value() && !controller.snapshot().hasSnapshot()) {
            return fatal("snapshot_not_found",
                         "the log has no complete snapshot with that number (" +
                             std::to_string(framesSeen_.load()) + " complete frames were read)",
                         kHeadlessExitSetup);
        }
        const bridge::Frame* frame = controller.snapshot().frame();
        if (frame != nullptr) {
            emitSnapshot(controller.snapshot().unreadable());
        }
        json ready = {
            {"event", "ready"},
            {"from_start", config_.options.fromStart},
            {"snapshot", frame != nullptr ? json(frame->snap) : json(nullptr)},
            {"date", frame != nullptr ? json(advisor::formatGameDate(frame->date)) : json(nullptr)},
            {"frames", controller.snapshot().sequence()},
            {"model", config_.model},
            {"effort", providers::toString(config_.options.effort)},
            {"thinking", providers::toString(config_.options.thinking)},
            {"persona_version", advisor::kPersonaVersion},
            {"transcripts",
             transcriptDir_.has_value() ? json(transcriptDir_->string()) : json(nullptr)}};
        writer_.write(std::move(ready));
        return 0;
    }

    // Commands run as they were read, except that an `ask` or `retry` waits for the question in
    // flight to end and does not hold up the commands behind it: a `cancel`, `quit`, `arm`,
    // `refresh` or `ck3` acts at once. (So "ask a / ask b / cancel" stops a and then runs b.)
    void processCommands() {
        auto it = pending_.begin();
        while (it != pending_.end() && !quit_) {
            const HeadlessCommand command = parseHeadlessCommand(*it);
            const bool isQuestion = command.kind == HeadlessCommand::Kind::Ask ||
                                    command.kind == HeadlessCommand::Kind::Retry;
            if (isQuestion && controller_->session().inFlight()) {
                ++it;  // waits; the loop wakes at the end of the question in flight
                continue;
            }
            pending_.erase(it);
            execute(command);
            // The command may have freed the way for a question that was skipped: look again.
            it = pending_.begin();
        }
    }

    void execute(const HeadlessCommand& command) {
        using Kind = HeadlessCommand::Kind;
        const bool inFlight = controller_->session().inFlight();
        switch (command.kind) {
        case Kind::Nothing:
            break;
        case Kind::Ask:
            refusedIfNotStarted(controller_->onUserAsk(command.text));
            break;
        case Kind::Retry:
            refusedIfNotStarted(controller_->onRetry());
            break;
        case Kind::Cancel:
            if (inFlight) {
                controller_->onStop();
            } else {
                writer_.write(json::object({{"event", "cancel_ignored"}}));
            }
            break;
        case Kind::Arm:
            controller_->onOverlayOpened(OverlaySource::Hotkey);
            writer_.write({{"event", "overlay_open"},
                           {"source", toString(OverlaySource::Hotkey)},
                           {"probe", false}});
            break;
        case Kind::Refresh:
            controller_->onOverlayOpened(OverlaySource::Refresh);
            writer_.write({{"event", "overlay_open"},
                           {"source", toString(OverlaySource::Refresh)},
                           {"probe", false}});
            break;
        case Kind::Ck3:
            controller_->onCk3Running(command.ck3Running);
            writer_.write({{"event", "ck3"}, {"running", command.ck3Running}});
            break;
        case Kind::Quit:
            quit_ = true;
            break;
        case Kind::Invalid:
            writer_.write({{"event", "bad_command"}, {"message", command.problem}});
            break;
        }
    }

    void refusedIfNotStarted(advisor::AskOutcome outcome) {
        if (outcome != advisor::AskOutcome::Started) {
            writer_.write({{"event", "ask_refused"}, {"outcome", advisor::toString(outcome)}});
        }
    }

    // ---- observer: the controller calls these on this thread, inside pump() ------------------

    void emitSnapshot(bool unreadable) {
        const AppController& controller = *controller_;
        const bridge::Frame* frame = controller.snapshot().frame();
        writer_.write({{"event", "snapshot"},
                       {"sequence", controller.snapshot().sequence()},
                       {"snap", frame != nullptr ? json(frame->snap) : json(nullptr)},
                       {"date", frame != nullptr ? json(advisor::formatGameDate(frame->date))
                                                 : json(nullptr)},
                       {"complete", !unreadable},
                       {"unreadable", unreadable},
                       {"probe", frame != nullptr && frame->mode == 1}});
    }

    void onSnapshotChanged(const SnapshotInfo& info) override {
        if (!ready_) {
            return;  // catching up with the log: only the newest snapshot is reported, at ready
        }
        emitSnapshot(info.unreadable);
    }

    void onOverlayRequested(bool probeExport) override {
        if (!ready_) {
            // Part of the log that was already there (a click in the start-up read is history).
            // A click written in the instant between the start-up read and the first poll falls
            // here too and is not reported; the next one is.
            return;
        }
        writer_.write({{"event", "overlay_open"},
                       {"source", toString(OverlaySource::GameClick)},
                       {"probe", probeExport}});
        // The game's own click is not a request by the player to refresh; nothing is armed.
        controller_->onOverlayOpened(OverlaySource::GameClick);
    }

    void onNoticeChanged(advisor::Notice notice) override {
        writer_.write({{"event", "notice"}, {"kind", noticeKind(notice)}});
    }

    void onUnsupportedFormat(std::string_view leadingFields) override {
        writer_.write(
            {{"event", "unsupported_format"}, {"leading_fields", std::string(leadingFields)}});
    }

    [[nodiscard]] const advisor::TranscriptEntry* answer() const {
        return controller_->session().currentAnswer();
    }

    // The user turn that the current answer belongs to.
    [[nodiscard]] const advisor::TranscriptEntry* question() const {
        const auto& transcript = controller_->session().transcript();
        const advisor::TranscriptEntry* current = answer();
        if (current == nullptr || transcript.empty()) {
            return nullptr;
        }
        auto index = static_cast<std::size_t>(current - transcript.data());
        while (index > 0) {
            --index;
            if (transcript[index].kind == advisor::EntryKind::User) {
                return &transcript[index];
            }
        }
        return nullptr;
    }

    void flushText() {
        const advisor::TranscriptEntry* current = answer();
        if (current == nullptr) {
            return;
        }
        const std::string& text = current->text;
        if (text.size() == emitted_.size() && text == emitted_) {
            return;
        }
        if (text.size() > emitted_.size() && text.compare(0, emitted_.size(), emitted_) == 0) {
            writer_.write({{"event", "token"}, {"text", text.substr(emitted_.size())}});
        } else {
            writer_.write({{"event", "text_reset"}, {"text", text}});
        }
        emitted_ = text;
        if (config_.options.selftestThrow && !text.empty() && testOnlyOptionsAllowed()) {
            // The fatal-error path, exercised on purpose (test builds only): the message
            // carries a key-shaped string, which the handler must not print.
            throw std::runtime_error("selftest exception holding a key-shaped value: sk-"
                                     "ant-"
                                     "FAKE"
                                     "selftest00000000000000000");
        }
    }

    void onTranscriptChanged(std::size_t /*index*/) override {
        if (!requestActive_) {
            return;
        }
        flushText();
        // Once per answer, and only before its first text: the basis of the chat view's
        // "thinking..." state.
        const advisor::TranscriptEntry* current = answer();
        if (!thinkingAnnounced_ && controller_->session().thinking() && current != nullptr &&
            current->firstTokenMs < 0) {
            thinkingAnnounced_ = true;
            writer_.write({{"event", "status"}, {"state", "thinking"}});
        }
    }

    void onChatState(advisor::ChatState state) override {
        using advisor::ChatState;
        switch (state) {
        case ChatState::Idle:
            break;
        case ChatState::Waiting: {
            requestActive_ = true;
            // The snapshot the question is asked with (a newer one may arrive while it streams).
            const bridge::Frame* held = controller_->snapshot().frame();
            requestSnap_ = held != nullptr ? held->snap : 0;
            requestDate_ = held != nullptr ? advisor::formatGameDate(held->date) : std::string();
            emitted_.clear();
            thinkingAnnounced_ = false;
            const advisor::TranscriptEntry* asked = question();
            writer_.write({{"event", "ask"},
                           {"generation", controller_->session().lastRequest().generation},
                           {"text", asked != nullptr ? asked->text : std::string()}});
            writer_.write({{"event", "status"}, {"state", "waiting"}});
            break;
        }
        case ChatState::Streaming:
            writer_.write({{"event", "status"}, {"state", "streaming"}});
            break;
        case ChatState::Done:
        case ChatState::Error:
        case ChatState::Cancelled:
            finishRequest(state);
            break;
        }
    }

    void finishRequest(advisor::ChatState state) {
        const advisor::TranscriptEntry* current = answer();
        if (current == nullptr || !requestActive_) {
            return;
        }
        requestActive_ = false;
        flushText();
        const std::uint64_t generation = controller_->session().lastRequest().generation;
        const advisor::TranscriptEntry& entry = *current;
        json usage = {{"input", entry.usage.input},
                      {"output", entry.usage.output},
                      {"cache_read", entry.usage.cacheRead},
                      {"cache_write", entry.usage.cacheWrite}};
        TranscriptRecord record;
        if (state == advisor::ChatState::Done) {
            json done = {{"event", "done"},
                         {"generation", generation},
                         {"text", entry.text},
                         {"first_token_ms", nullableMs(entry.firstTokenMs)},
                         {"total_ms", nullableMs(entry.totalMs)},
                         {"usage", usage},
                         {"stop_reason", entry.usage.stopReason},
                         {"max_tokens", entry.hitTokenLimit},
                         {"effort", providers::toString(config_.options.effort)},
                         {"thinking", providers::toString(config_.options.thinking)}};
            if (entry.thinkingStartedMs >= 0) {
                done["thinking_started_ms"] = entry.thinkingStartedMs;
            }
            writer_.write(std::move(done));
            record.end = "complete";
        } else if (state == advisor::ChatState::Cancelled) {
            writer_.write({{"event", "cancelled"},
                           {"generation", generation},
                           {"text", entry.text},
                           {"total_ms", nullableMs(entry.totalMs)}});
            record.end = "stopped";
        } else {
            anyFailed_ = true;
            const advisor::ErrorInfo info = entry.error.value_or(advisor::ErrorInfo{});
            writer_.write(
                {{"event", "error"},
                 {"generation", generation},
                 {"code", providers::toString(info.code)},
                 {"message", info.summary},
                 {"detail", info.detail},
                 {"retryable", info.retryable},
                 {"http_status", info.httpStatus},
                 {"retry_after", info.retryAfterSeconds.has_value() ? json(*info.retryAfterSeconds)
                                                                    : json(nullptr)},
                 {"request_id", info.requestId},
                 {"partial_len", entry.text.size()},
                 {"partial_text", entry.text},
                 {"total_ms", nullableMs(entry.totalMs)}});
            record.end = "failed";
            record.errorCode = providers::toString(info.code);
            record.errorSummary = info.summary;
        }
        saveTranscript(record, entry, generation);
    }

    void saveTranscript(TranscriptRecord& record, const advisor::TranscriptEntry& entry,
                        std::uint64_t generation) {
        if (!transcriptDir_.has_value()) {
            return;
        }
        const advisor::TranscriptEntry* asked = question();
        record.model = config_.model;
        record.effort = providers::toString(config_.options.effort);
        record.thinking = providers::toString(config_.options.thinking);
        record.snap = requestSnap_;
        record.gameDate = requestDate_;
        record.generation = generation;
        record.personaVersion = advisor::kPersonaVersion;
        record.question = asked != nullptr ? asked->text : std::string();
        record.answer = entry.text;
        record.input = entry.usage.input;
        record.output = entry.usage.output;
        record.cacheRead = entry.usage.cacheRead;
        record.cacheWrite = entry.usage.cacheWrite;
        record.stopReason = entry.usage.stopReason;
        record.firstTokenMs = entry.firstTokenMs;
        record.totalMs = entry.totalMs;
        std::string error;
        const std::optional<std::filesystem::path> path =
            curia::writeTranscript(*transcriptDir_, record, error, config_.registry);
        if (path.has_value()) {
            writer_.write({{"event", "transcript"}, {"path", path->string()}});
        } else {
            writer_.write({{"event", "transcript_error"}, {"message", error}});
        }
    }

    HeadlessChatConfig config_;
    std::istream& in_;
    std::ostream& err_;
    JsonLineWriter writer_;
    std::shared_ptr<Signal> signal_;

    std::optional<std::filesystem::path> transcriptDir_;
    std::uint64_t initialSize_ = 0;
    std::atomic<std::uint64_t> progressOffset_{0};
    std::atomic<bool> progressSeen_{false};
    std::atomic<int> framesSeen_{0};

    bool ready_ = false;
    bool quit_ = false;
    bool inputEnded_ = false;
    bool anyFailed_ = false;
    bool requestActive_ = false;
    bool thinkingAnnounced_ = false;
    std::int32_t requestSnap_ = 0;
    std::string requestDate_;
    std::string emitted_;
    std::deque<std::string> pending_;
    std::thread reader_;

    // Declared last among the parts that others depend on: the watcher is stopped first, then
    // the controller (which cancels a request in flight) goes, then the provider is released.
    std::unique_ptr<AppController> controller_;
    std::unique_ptr<bridge::LogWatcher> watcher_;
};

}  // namespace

bool testOnlyOptionsAllowed() {
#if defined(CURIA_ENABLE_DEV_ENV)
    return true;
#else
    return false;
#endif
}

std::string describeFatalException(const char* what, const SecretRegistry* registry) {
    std::string text = what != nullptr ? what : "";
    text = registry != nullptr ? registry->redact(text) : redact(text);
    if (text.size() > kMaxFatalBytes) {
        std::size_t cut = kMaxFatalBytes;
        while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0U) == 0x80U) {
            --cut;
        }
        text.resize(cut);
    }
    for (char& ch : text) {
        if (static_cast<unsigned char>(ch) < 0x20 || ch == 0x7f) {
            ch = '?';
        }
    }
    return "internal error: " + text;
}

HeadlessChatResult runHeadlessChat(HeadlessChatConfig config, std::istream& in, std::ostream& out,
                                   std::ostream& err) {
    Runner runner(std::move(config), in, out, err);
    return runner.run();
}

}  // namespace curia
