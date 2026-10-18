// SPDX-License-Identifier: MIT
#include "app.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "bridge/log_tailer.h"
#include "bridge/log_watcher.h"
#include "chatview/log_path.h"
#include "core/app_controller.h"
#include "core/app_log.h"
#include "core/clock.h"
#include "core/dev_config.h"
#include "core/game_poll.h"
#include "core/ui_pacing.h"
#include "headless/headless_modes.h"
#include "platform/platform.h"
#include "ui/chat_window.h"
#include "ui/hotkey.h"
#include "ui/overlay.h"
#include "ui/render_backend.h"
#include "ui/sdl_wake.h"
#include "ui/smoke_run.h"
#include "ui/theme.h"
#include "ui/tray.h"

namespace curia {

namespace {

// Frames drawn after a real event (enough for ImGui to settle hover and layout state) and after a
// change on screen (the scroll position follows a frame later).
constexpr int kFramesAfterEvent = 2;
constexpr int kQuietTicksBeforeBaseline = 2;
constexpr int kMaxTicksToSettle = 30;
// The once-a-second tick of the log-silence monitor, of the CK3-running poll (and of the idle
// test).
constexpr Uint32 kTickMs = 1000;
// How long after a show or a hide the overlay settles (ui::Overlay::settle(): the focus is logged
// as it then is, and after a hide the keys are handed back if Curia is still the active app).
constexpr Uint32 kSettleMs = 250;
// The window, in logical pixels (times the display's content scale where the OS counts window
// sizes in pixels); the panel shrinks towards the minimum on a small CK3 window.
constexpr int kDefaultWidth = 520;
constexpr int kDefaultHeight = 680;
constexpr int kMinWidth = 340;
constexpr int kMinHeight = 420;

// State of the --idle-test mode (all of it is touched on the main thread only; the timer thread
// only pushes events).
struct IdleTest {
    int ticks = 0;
    int quietTicks = 0;
    bool eventSinceTick = false;
    bool baselineSet = false;
    std::uint64_t baselineFrames = 0;
    int ticksAfterBaseline = 0;
    int externalEventsAfterBaseline = 0;
    std::string lastEventTypes;  // diagnostics, printed on failure
};

// The event types of our own. The timers, the mailbox, the hot key and the tray menu only push one
// of these; the run loop acts on them.
struct AppEvents {
    Uint32 wake = 0;    // the mailbox has messages
    Uint32 tick = 0;    // once a second
    Uint32 hotkey = 0;  // Ctrl+Cmd+K
    Uint32 tray = 0;    // a tray entry (event.user.code is ui::Tray::Entry)
    Uint32 settle = 0;  // kSettleMs after a show or a hide (event.user.code: its generation)
};
constexpr int kAppEventCount = 5;

// The timer's userdata is the event type itself, not a pointer to it: SDL_RemoveTimer() does not
// wait for a callback that is running on SDL's timer thread, so there must be nothing to outlive.
void* tickUserdata(Uint32 eventType) {
    return reinterpret_cast<void*>(static_cast<std::uintptr_t>(eventType));
}

Uint32 SDLCALL tickCallback(void* userdata, SDL_TimerID /*timerId*/, Uint32 interval) {
    SDL_Event event;
    SDL_zero(event);
    event.type = static_cast<Uint32>(reinterpret_cast<std::uintptr_t>(userdata));
    SDL_PushEvent(&event);
    return interval;
}

// The settle timer's userdata, for the same reason: the event type in the low 16 bits (SDL's
// event types end at SDL_EVENT_LAST, 0xFFFF) and the generation of the show or hide above them.
constexpr Uint32 kSettleGenerationMask = 0x7FFFU;
static_assert(SDL_EVENT_LAST <= 0xFFFF);

void* settleUserdata(Uint32 eventType, Uint32 generation) {
    return reinterpret_cast<void*>(
        (static_cast<std::uintptr_t>(generation & kSettleGenerationMask) << 16U) |
        static_cast<std::uintptr_t>(eventType));
}

Uint32 SDLCALL settleCallback(void* userdata, SDL_TimerID /*timerId*/, Uint32 /*interval*/) {
    const auto packed = reinterpret_cast<std::uintptr_t>(userdata);
    SDL_Event event;
    SDL_zero(event);
    event.type = static_cast<Uint32>(packed & 0xFFFFU);
    event.user.code = static_cast<Sint32>(packed >> 16U);
    SDL_PushEvent(&event);
    return 0;  // once
}

// A hidden overlay, or a hidden, minimised or fully covered window, draws nothing (drawing into a
// covered Metal layer can block). The overlay's own answer counts too: SDL learns of orderOut only
// through its observation of the panel. After the panel is shown again SDL still reports it
// covered (SDL_WINDOW_OCCLUDED from the earlier orderOut) until AppKit's occlusion notice arrives,
// some 15 ms later as SDL_EVENT_WINDOW_EXPOSED: that event draws the first new frame, and until
// then the panel shows its last frame from before it was hidden.
bool windowHidden(const ui::Overlay& overlay) {
    return !overlay.visible() ||
           (SDL_GetWindowFlags(overlay.window()) &
            (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0;
}

// What the chat window gets from outside before any window opens.
struct WindowSetup {
    std::unique_ptr<AppLog> log;
    std::shared_ptr<providers::Provider> provider;
    std::string model;
    // Why there is no provider (names variables only, never a value).
    std::string notConfigured;
    chatview::LogPathChoice logPath;
    // CK3's user folder is under Documents on this OS (the folder-not-found text says so).
    bool ck3FolderInDocuments = true;
    // The platform layer (is CK3 running, where is its window).
    std::unique_ptr<platform::Platform> platform;
};

// Reads the development configuration, builds the provider as the headless chat does, and picks
// the game log. False (a message on stderr) for a refused --test-endpoint: a usage error, exit 2.
bool prepareWindow(const CliOptions& options, WindowSetup& setup) {
    DevConfig config = loadDevConfig();
    for (const std::string& warning : config.warnings) {
        std::fprintf(stderr, "curia: note: %s\n", warning.c_str());
    }
    if (config.logContent) {
        std::fprintf(stderr, "curia: note: %s\n", std::string(logContentWindowNote()).c_str());
    }
    setup.log = headless::openAppLog(options);
    // Which development variables reached this process, as words (never a value): how the owner
    // tells whether `open --env NAME` forwarded a variable to Curia.app. Before makeProvider,
    // which moves the key out of the configuration.
    const DevConfigPresence presence = devConfigPresence(config);
    if (setup.log != nullptr) {
        setup.log->log(AppLog::Level::Info, "dev_config",
                       {{"key", presence.key}, {"model", presence.model}});
    }
    headless::ProviderSetup provider = headless::makeProvider(options, config, setup.log.get());
    if (!provider.error.empty()) {
        std::fprintf(stderr, "curia: %s\n", provider.error.c_str());
        return false;
    }
    if (provider.provider == nullptr) {
        setup.notConfigured = describeNotConfigured(config);
    }
    setup.provider = std::move(provider.provider);
    setup.model = std::move(provider.model);
    setup.platform = platform::create();
    // Windows and macOS keep it in Documents; Linux (native or Proton) in one of several places.
    setup.ck3FolderInDocuments = setup.platform->name() != "linux";
    setup.logPath = chatview::chooseLogPath(options.logPath, setup.platform->candidateCk3UserDirs(),
                                            [](const std::filesystem::path& path) {
                                                std::error_code ec;
                                                return std::filesystem::is_regular_file(path, ec);
                                            });
    return true;
}

// Whether CK3 runs, asked of the platform layer on the tick (window mode only), with the cost of
// the last question for the app log.
class Ck3Poll {
public:
    explicit Ck3Poll(const platform::Platform& os)
        : poll_([this, &os] { return ask(os); }, os.gameRunningPollTicks()) {}

    // On a change, tells the controller and logs it.
    void onTick(AppController& controller, AppLog* log) {
        const std::optional<bool> changed = poll_.onTick();
        if (!changed.has_value()) {
            return;
        }
        const bool running = changed.value_or(false);
        if (log != nullptr) {
            log->log(AppLog::Level::Info, "ck3_running",
                     {{"running", running}, {"poll_us", lastMicros_}});
        }
        controller.onCk3Running(running);
    }

private:
    std::optional<bool> ask(const platform::Platform& os) {
        const auto start = std::chrono::steady_clock::now();
        const std::optional<bool> answer = os.isGameRunning();
        lastMicros_ = std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - start)
                          .count();
        return answer;
    }

    long long lastMicros_ = 0;
    GamePoll poll_;
};

// Everything the event loop works with.
struct Loop {
    ui::Overlay& overlay;
    ui::IRenderBackend& backend;
    AppController& controller;
    ui::ChatWindow& chat;
    AppEvents events;
    int idleSeconds = 0;
    // Window mode only.
    Ck3Poll* poll = nullptr;
    AppLog* log = nullptr;
    // The generation of the last show or hide; only its settle event counts.
    Uint32 settleGeneration = 0;
    // The first key press after a show is logged (that it came, never which key).
    bool keyAfterShow = false;
};

// The overlay settles kSettleMs from now (only the newest request counts). Without the timer the
// focus is not logged and a hide gets no help with the keys; nothing else depends on it.
void requestSettle(Loop& loop) {
    loop.settleGeneration = (loop.settleGeneration + 1) & kSettleGenerationMask;
    SDL_AddTimer(kSettleMs, settleCallback,
                 settleUserdata(loop.events.settle, loop.settleGeneration));
}

// Shows the overlay (placed, in front, keys by the focus mode) with the input box focused. The
// chat window only.
void showOverlay(Loop& loop, ui::ShowReason reason) {
    loop.overlay.show(reason);
    loop.chat.focusInput();
    loop.keyAfterShow = true;
    requestSettle(loop);
}

// Hides the overlay (Esc, the hot key, the tray). The chat window only.
void hideOverlay(Loop& loop) {
    loop.overlay.hide();
    loop.keyAfterShow = false;
    requestSettle(loop);
}

// A key press that reaches the window: not a modifier alone (Ctrl and Cmd of the hot key's chord
// arrive as key events of their own), or typed text.
bool isTypedKey(const SDL_Event& event) {
    if (event.type == SDL_EVENT_TEXT_INPUT) {
        return true;
    }
    return event.type == SDL_EVENT_KEY_DOWN &&
           !(event.key.scancode >= SDL_SCANCODE_LCTRL && event.key.scancode <= SDL_SCANCODE_RGUI);
}

// The event loop of the chat window and of --idle-test (idleSeconds > 0). Event-driven: it blocks
// in SDL_WaitEvent and draws only after a real event (kFramesAfterEvent frames), after a change on
// screen that the controller reported, or on the short timeouts of core/ui_pacing.h while a dot
// pulses (thinking, or an answer arriving) or the input box has the keyboard (its caret blinks).
// The mailbox's wake-up event leads to a pump, the tick event to the monitor's tick and the
// CK3-running poll, the settle event to ui::Overlay::settle(); none draws by itself. The hot key
// toggles the overlay, the tray shows, hides or quits, Esc hides; a hidden overlay draws nothing.
// The window's focus changes and the first typed key after a show are logged (words only).
int runEventLoop(Loop& loop) {
    std::uint64_t frames = 0;
    bool running = true;
    int exitCode = 0;
    IdleTest idle;
    const bool idleMode = loop.idleSeconds > 0;
    int redraw = kFramesAfterEvent;
    SDL_Window* window = loop.overlay.window();

    while (running) {
        // A hidden window draws nothing, whatever is going on (events are still handled).
        const bool hidden = windowHidden(loop.overlay);
        const bool typing = ImGui::GetIO().WantTextInput &&
                            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        bool timedOut = false;
        SDL_Event event;
        bool have = false;
        if (redraw > 0 && !hidden) {
            have = SDL_PollEvent(&event);
        } else {
            const WaitPolicy policy = uiWaitPolicy(typing, loop.chat.spinnerActive(), hidden,
                                                   !loop.controller.mailbox()->empty());
            if (policy.kind == WaitPolicy::Kind::Block) {
                if (!SDL_WaitEvent(&event)) {
                    std::fprintf(stderr, "SDL_WaitEvent failed: %s\n", SDL_GetError());
                    exitCode = 1;
                    break;
                }
                have = true;
            } else {
                have = SDL_WaitEventTimeout(&event, policy.timeoutMs);
                timedOut = !have;
            }
        }

        bool wake = false;
        bool tick = false;
        bool hotkey = false;
        bool settle = false;
        std::vector<Sint32> trayEntries;
        while (have) {
            if (event.type == loop.events.wake) {
                wake = true;  // internal: the mailbox has messages
            } else if (event.type == loop.events.settle) {
                // Internal; an older show's or hide's settle event is dropped.
                settle = settle || static_cast<Uint32>(event.user.code) == loop.settleGeneration;
            } else if (event.type == loop.events.hotkey) {
                hotkey = true;  // only registered in the chat window
            } else if (event.type == loop.events.tray) {
                trayEntries.push_back(event.user.code);  // likewise
            } else if (event.type == loop.events.tick) {
                tick = true;  // internal: must not cause a redraw by itself
                if (idleMode) {
                    ++idle.ticks;
                    if (!idle.baselineSet) {
                        idle.quietTicks =
                            (!idle.eventSinceTick && redraw == 0) ? idle.quietTicks + 1 : 0;
                        if (idle.quietTicks >= kQuietTicksBeforeBaseline) {
                            idle.baselineSet = true;
                            idle.baselineFrames = frames;
                        } else if (idle.ticks > kMaxTicksToSettle) {
                            std::fprintf(
                                stderr,
                                "CURIA_IDLE_FAIL the window never became quiet (events: %s)\n",
                                idle.lastEventTypes.c_str());
                            exitCode = 1;
                            running = false;
                        }
                    } else if (++idle.ticksAfterBaseline >= loop.idleSeconds) {
                        running = false;
                    }
                    idle.eventSinceTick = false;
                }
            } else {
                loop.backend.processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;  // the plain window's close button quits (it does not hide)
                }
                // For the focus questions of the M4 testing (E6): nothing is logged without an
                // app log, so not in the test modes.
                if (event.type == SDL_EVENT_WINDOW_FOCUS_GAINED) {
                    loop.overlay.logFocus("focus_gained");
                } else if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    loop.overlay.logFocus("focus_lost");
                } else if (loop.keyAfterShow && isTypedKey(event)) {
                    loop.keyAfterShow = false;
                    loop.overlay.logFocus("first_key");
                }
                redraw = kFramesAfterEvent;
                if (idleMode) {
                    idle.eventSinceTick = true;
                    if (idle.baselineSet) {
                        ++idle.externalEventsAfterBaseline;
                    }
                    if (idle.lastEventTypes.size() < 80) {
                        idle.lastEventTypes += " 0x" + std::to_string(event.type);
                    }
                }
            }
            have = SDL_PollEvent(&event);
        }

        if (wake || !loop.controller.mailbox()->empty()) {
            loop.controller.pump();
        }
        if (tick) {
            loop.controller.onTick(steadyNow());
            if (loop.poll != nullptr) {
                loop.poll->onTick(loop.controller, loop.log);
            }
        }
        if (settle && loop.overlay.settle()) {
            requestSettle(loop);  // it asked another app to take the keys: log how that went
        }
        if (loop.chat.takeOverlayRequest()) {
            // The player clicked Advisor in the game: the overlay comes up on CK3's display with
            // the input ready (the panel without activating Curia).
            showOverlay(loop, ui::ShowReason::GameClick);
            loop.controller.onOverlayOpened(OverlaySource::GameClick);
            redraw = kFramesAfterEvent;
        }
        if (hotkey) {
            if (loop.overlay.visible()) {
                hideOverlay(loop);
            } else {
                showOverlay(loop, ui::ShowReason::Hotkey);
                loop.controller.onOverlayOpened(OverlaySource::Hotkey);  // arms the detector
                redraw = kFramesAfterEvent;
            }
        }
        for (const Sint32 entry : trayEntries) {
            if (entry == ui::Tray::Show) {
                showOverlay(loop, ui::ShowReason::Tray);
                loop.controller.onOverlayOpened(OverlaySource::Tray);
                redraw = kFramesAfterEvent;
            } else if (entry == ui::Tray::Hide) {
                hideOverlay(loop);
            } else if (entry == ui::Tray::Quit) {
                running = false;
            }
        }
        if (loop.chat.takeHideRequest() && !idleMode) {
            // Esc. The question in the box stays, an answer keeps arriving (FR-OVL-4).
            hideOverlay(loop);
        }
        if (loop.chat.takeChanged()) {
            redraw = std::max(redraw, kFramesAfterEvent);
        }
        if (timedOut && (loop.chat.spinnerActive() || typing)) {
            redraw = std::max(redraw, 1);  // one animation or caret frame
        }
        if (running && redraw > 0) {
            // Asked again: the events just handled may have shown, restored or uncovered the
            // window (SDL updates the flags as it delivers them), and that event's frames must be
            // drawn, not thrown away with a state from before the wait.
            if (windowHidden(loop.overlay)) {
                redraw = 0;
            } else {
                loop.backend.beginFrame();
                loop.chat.draw();
                loop.backend.endFrame();
                ++frames;
                --redraw;
            }
        }
    }

    if (idleMode && exitCode == 0) {
        // Frames after the baseline are drawn only after an event, so every one of them must be
        // explained by a real (non-timer) event, at most kFramesAfterEvent each. The internal
        // events (the tick, the wake-up) change nothing on screen in this mode: no provider and
        // no log are attached, and there is no poll, hot key, tray or settle timer.
        const auto allowed =
            idle.baselineFrames +
            static_cast<std::uint64_t>(kFramesAfterEvent * idle.externalEventsAfterBaseline);
        const bool idleOk = idle.baselineSet && frames <= allowed;
        std::printf("%s baseline_frames=%llu final_frames=%llu external_events=%d seconds=%d "
                    "window=%s%s%s\n",
                    idleOk ? "CURIA_IDLE_OK" : "CURIA_IDLE_FAIL",
                    static_cast<unsigned long long>(idle.baselineFrames),
                    static_cast<unsigned long long>(frames), idle.externalEventsAfterBaseline,
                    loop.idleSeconds, loop.overlay.kind(),
                    idleOk ? "" : " events:", idleOk ? "" : idle.lastEventTypes.c_str());
        if (!idleOk) {
            exitCode = 1;
        }
    }
    return exitCode;
}

// The chat window (and --idle-test): the controller, the provider, the log watcher, in the chat
// window also the tray, the hot key and the CK3-running poll, and the loop.
int runWindow(const CliOptions& options, WindowSetup& setup, ui::Overlay& overlay,
              ui::IRenderBackend& backend, const ui::FontSet& fonts, const AppEvents& events) {
    const bool idleMode = options.idleSeconds > 0;
    ui::SdlWake wake(events.wake);
    AppController::Options controllerOptions;
    controllerOptions.log = setup.log.get();
    controllerOptions.wake = wake.function();
    auto controller = std::make_unique<AppController>(std::move(controllerOptions));
    if (setup.provider != nullptr) {
        controller->setProvider(setup.provider, setup.model);
    } else if (!setup.notConfigured.empty()) {
        controller->setNotConfigured(setup.notConfigured);
    }

    // The idle test reads nothing: no folder lookup, no log, no provider (deterministic).
    ui::ChatWindow::Options windowOptions;
    windowOptions.gameFolderFound = idleMode || !setup.logPath.path.empty();
    windowOptions.ck3FolderInDocuments = setup.ck3FolderInDocuments;
    auto chat = std::make_unique<ui::ChatWindow>(*controller, fonts, windowOptions);
    if (setup.log != nullptr) {
        setup.log->log(AppLog::Level::Info, "window_start",
                       {{"pid", setup.platform != nullptr ? setup.platform->processId() : 0LL},
                        {"configured", controller->configured()},
                        {"log_found", !setup.logPath.path.empty()},
                        {"log_from_flag", setup.logPath.fromFlag},
                        {"window", toString(options.window)},
                        {"focus", toString(options.overlayFocus)}});
    }

    std::unique_ptr<bridge::LogWatcher> watcher;
    if (!idleMode && !setup.logPath.path.empty()) {
        // From the end of the file: the realm appears when the player clicks Advisor after Curia
        // started (chatview/log_path.h).
        bridge::LogTailer::Options tailerOptions;
        tailerOptions.fromStart = false;
        auto tailer = std::make_unique<bridge::LogTailer>(
            std::make_unique<bridge::FileSource>(setup.logPath.path), tailerOptions);
        watcher =
            std::make_unique<bridge::LogWatcher>(std::move(tailer), controller->watcherCallbacks());
        watcher->start();
    }

    // The chat window only: the tray (created now or never: SDL_CreateTray activates the app on
    // macOS, so not at all while CK3 is in front, FR-OVL-6), the hot key (macOS) and the
    // CK3-running poll. The idle test has none of them.
    std::unique_ptr<ui::Tray> tray;
    std::unique_ptr<ui::HotKey> hotKey;
    std::unique_ptr<Ck3Poll> poll;
    if (!idleMode) {
        std::string_view trayStatus = "skipped_game_front";
        const std::optional<bool> gameFront =
            setup.platform != nullptr ? setup.platform->gameIsFrontmost() : std::nullopt;
        if (!gameFront.value_or(false)) {
            tray = ui::Tray::create(events.tray, trayStatus);
        }
        std::string_view hotkeyStatus;
        hotKey = ui::HotKey::install(events.hotkey, true, hotkeyStatus);
        if (setup.log != nullptr) {
            setup.log->log(AppLog::Level::Info, "controls",
                           {{"tray", trayStatus}, {"hotkey", hotkeyStatus}});
        }
        if (setup.platform != nullptr) {
            poll = std::make_unique<Ck3Poll>(*setup.platform);
        }
    }

    Loop loop{overlay,    backend,        *controller, *chat, events, options.idleSeconds,
              poll.get(), setup.log.get()};
    // Shown once at start-up, so the player sees that Curia started (on CK3's display if CK3
    // runs, else on the pointer's). The plain window of the idle test is already open as it was
    // created; the panel is not.
    if (!idleMode) {
        showOverlay(loop, ui::ShowReason::Start);
    } else if (options.usesPanel()) {
        overlay.show(ui::ShowReason::Start);
    }
    if (poll != nullptr) {
        poll->onTick(*controller, setup.log.get());  // the first question, at start-up
    }

    const SDL_TimerID timer = SDL_AddTimer(kTickMs, tickCallback, tickUserdata(events.tick));
    int exitCode = 0;
    if (timer == 0) {
        std::fprintf(stderr, "%sSDL_AddTimer failed: %s\n", idleMode ? "CURIA_IDLE_FAIL " : "",
                     SDL_GetError());
        exitCode = 1;
    } else {
        exitCode = runEventLoop(loop);
        SDL_RemoveTimer(timer);
    }

    // Shut down in order: nothing reads the log any more, the request in flight is stopped,
    // no event is pushed any more, then the controller (and with it the provider, whose
    // destructor joins its net thread) goes. The hot key and the tray go before SDL_Quit.
    if (watcher != nullptr) {
        watcher->stop();
    }
    controller->onStop();
    hotKey.reset();
    tray.reset();
    wake.disarm();
    chat.reset();
    controller.reset();
    setup.provider.reset();
    return exitCode;
}

}  // namespace

int runApp(const CliOptions& options) {
    // The chat window reads the configuration and may refuse the command line before any window
    // opens. The smoke and idle tests read nothing from the environment.
    WindowSetup setup;
    if (options.windowMode() && !prepareWindow(options, setup)) {
        return 2;
    }

    ui::prepareOverlayBeforeInit(options.window);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (options.smoke) {
        // CI machines may have no GPU; the software renderer exercises the same code path.
        // SDL_HINT_DEFAULT keeps a user-provided SDL_RENDER_DRIVER environment variable in charge.
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_DEFAULT);
    }

    ui::OverlayOptions overlayOptions;
    overlayOptions.width = options.windowWidth > 0 ? options.windowWidth : kDefaultWidth;
    overlayOptions.height = options.windowHeight > 0 ? options.windowHeight : kDefaultHeight;
    overlayOptions.minWidth = kMinWidth;
    overlayOptions.minHeight = kMinHeight;
    overlayOptions.focus = options.overlayFocus;
    // The test modes place the panel on the pointer's display, whatever runs on the machine.
    overlayOptions.followGame = options.windowMode();
    overlayOptions.alwaysOnTop = options.windowMode();
    overlayOptions.platform = setup.platform.get();
    overlayOptions.log = setup.log.get();
    std::unique_ptr<ui::Overlay> overlay = ui::createOverlay(options.window, overlayOptions);
    if (overlay == nullptr) {
        std::fprintf(stderr, "%sSDL_CreateWindow failed: %s\n",
                     options.idleSeconds > 0 ? "CURIA_IDLE_FAIL " : "", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_Window* window = overlay->window();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the binary
    // The content scale only (the DPI scale on Windows and Linux, 1.0 on macOS). The pixel density
    // is already applied through the framebuffer scale by the renderer backend, so using
    // SDL_GetWindowDisplayScale (density times content scale) here would draw the UI twice as
    // large on a Retina Mac.
    float uiScale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (uiScale <= 0.0F) {
        uiScale = 1.0F;
    }
    const ui::FontSet fonts = ui::loadFonts();
    ui::applyTheme(uiScale, fonts);

    auto backend = ui::createSdlRendererBackend();
    if (!backend->init(window)) {
        ImGui::DestroyContext();
        overlay.reset();
        SDL_Quit();
        return 1;
    }
    const ui::Rgb clear = ui::palette::kParchment;
    backend->setClearColor(clear.r, clear.g, clear.b, 255);

    int exitCode = 0;
    // The event types of our own: the mailbox's wake-up, the tick, the hot key, the tray and the
    // settle timer.
    const Uint32 firstEvent = SDL_RegisterEvents(kAppEventCount);
    if (firstEvent == 0) {
        std::fprintf(stderr, "%sSDL_RegisterEvents failed: %s\n",
                     options.idleSeconds > 0 ? "CURIA_IDLE_FAIL " : "", SDL_GetError());
        exitCode = 1;
    } else if (options.smoke) {
        if (options.usesPanel()) {
            overlay->show(ui::ShowReason::Start);  // the panel is drawn only once it is shown
        }
        ui::SmokeContext smoke;
        smoke.window = window;
        smoke.backend = backend.get();
        smoke.fonts = fonts;
        smoke.uiScale = uiScale;
        smoke.wakeEvent = firstEvent;
        smoke.screenshotPath = options.screenshotPath;
        smoke.scene = options.smokeScene;
        smoke.windowKind = overlay->kind();
        exitCode = ui::runSmoke(smoke);
    } else {
        const AppEvents events{firstEvent, firstEvent + 1, firstEvent + 2, firstEvent + 3,
                               firstEvent + 4};
        exitCode = runWindow(options, setup, *overlay, *backend, fonts, events);
    }

    // The renderer first, then the window (the panel after the SDL window that wraps it).
    backend->shutdown();
    backend.reset();
    ImGui::DestroyContext();
    overlay.reset();
    SDL_Quit();
    return exitCode;
}

}  // namespace curia
