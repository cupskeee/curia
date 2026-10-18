// SPDX-License-Identifier: MIT
#include "app.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#include "bridge/log_tailer.h"
#include "bridge/log_watcher.h"
#include "chatview/log_path.h"
#include "core/app_controller.h"
#include "core/app_log.h"
#include "core/clock.h"
#include "core/dev_config.h"
#include "core/ui_pacing.h"
#include "headless/headless_modes.h"
#include "platform/platform.h"
#include "ui/chat_window.h"
#include "ui/render_backend.h"
#include "ui/sdl_wake.h"
#include "ui/smoke_run.h"
#include "ui/theme.h"

namespace curia {

namespace {

// Frames drawn after a real event (enough for ImGui to settle hover and layout state) and after a
// change on screen (the scroll position follows a frame later).
constexpr int kFramesAfterEvent = 2;
constexpr int kQuietTicksBeforeBaseline = 2;
constexpr int kMaxTicksToSettle = 30;
// The once-a-second tick of the log-silence monitor (and of the idle test).
constexpr Uint32 kTickMs = 1000;
// The window, in logical pixels (times the display's content scale where the OS counts window
// sizes in pixels).
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

// A hidden, minimised or fully covered window draws nothing (drawing into a covered Metal layer
// can block).
bool windowHidden(SDL_Window* window) {
    return (SDL_GetWindowFlags(window) &
            (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED | SDL_WINDOW_OCCLUDED)) != 0;
}

// Window sizes in the units SDL_CreateWindow takes. macOS and Wayland (by default) count in points
// and report a content scale of 1; Windows and X11 count in pixels and report the DPI scale, so a
// size in logical pixels is multiplied by it there.
int toWindowUnits(int logical, float contentScale) {
    return static_cast<int>(std::lround(static_cast<float>(logical) * contentScale));
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
    const std::unique_ptr<platform::Platform> os = platform::create();
    // Windows and macOS keep it in Documents; Linux (native or Proton) in one of several places.
    setup.ck3FolderInDocuments = os->name() != "linux";
    setup.logPath = chatview::chooseLogPath(options.logPath, os->candidateCk3UserDirs(),
                                            [](const std::filesystem::path& path) {
                                                std::error_code ec;
                                                return std::filesystem::is_regular_file(path, ec);
                                            });
    return true;
}

// The event loop of the chat window and of --idle-test (idleSeconds > 0). Event-driven: it blocks
// in SDL_WaitEvent and draws only after a real event (kFramesAfterEvent frames), after a change on
// screen that the controller reported, or on the short timeouts of core/ui_pacing.h while a dot
// pulses (thinking, or an answer arriving) or the input box has the keyboard (its caret blinks).
// The mailbox's wake-up event leads to a pump, the tick event to the monitor's tick; neither draws
// by itself.
int runEventLoop(SDL_Window* window, ui::IRenderBackend& backend, AppController& controller,
                 ui::ChatWindow& chat, Uint32 wakeEvent, Uint32 tickEvent, int idleSeconds) {
    std::uint64_t frames = 0;
    bool running = true;
    int exitCode = 0;
    IdleTest idle;
    const bool idleMode = idleSeconds > 0;
    int redraw = kFramesAfterEvent;

    while (running) {
        // A hidden window draws nothing, whatever is going on (events are still handled).
        const bool hidden = windowHidden(window);
        const bool typing = ImGui::GetIO().WantTextInput &&
                            (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
        bool timedOut = false;
        SDL_Event event;
        bool have = false;
        if (redraw > 0 && !hidden) {
            have = SDL_PollEvent(&event);
        } else {
            const WaitPolicy policy =
                uiWaitPolicy(typing, chat.spinnerActive(), hidden, !controller.mailbox()->empty());
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
        while (have) {
            if (event.type == wakeEvent) {
                wake = true;  // internal: the mailbox has messages
            } else if (event.type == tickEvent) {
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
                    } else if (++idle.ticksAfterBaseline >= idleSeconds) {
                        running = false;
                    }
                    idle.eventSinceTick = false;
                }
            } else {
                backend.processEvent(event);
                if (event.type == SDL_EVENT_QUIT ||
                    event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
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

        if (wake || !controller.mailbox()->empty()) {
            controller.pump();
        }
        if (tick) {
            controller.onTick(steadyNow());
        }
        if (chat.takeOverlayRequest()) {
            // The player clicked Advisor in the game: bring the window forward with the input
            // ready. Only a minimised window is restored (SDL_RestoreWindow would also shrink a
            // maximised one). TODO(PR 8): the overlay panel shows without activating Curia; a
            // normal window that is raised activates the app (on macOS that leaves CK3's
            // fullscreen Space).
            if ((SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) != 0) {
                SDL_RestoreWindow(window);
            }
            SDL_RaiseWindow(window);
            chat.focusInput();
            controller.onOverlayOpened(OverlaySource::GameClick);
        }
        if (chat.takeChanged()) {
            redraw = std::max(redraw, kFramesAfterEvent);
        }
        if (timedOut && (chat.spinnerActive() || typing)) {
            redraw = std::max(redraw, 1);  // one animation or caret frame
        }
        if (running && redraw > 0) {
            // Asked again: the events just handled may have shown, restored or uncovered the
            // window (SDL updates the flags as it delivers them), and that event's frames must be
            // drawn, not thrown away with a state from before the wait.
            if (windowHidden(window)) {
                redraw = 0;
            } else {
                backend.beginFrame();
                chat.draw();
                backend.endFrame();
                ++frames;
                --redraw;
            }
        }
    }

    if (idleMode && exitCode == 0) {
        // Frames after the baseline are drawn only after an event, so every one of them must be
        // explained by a real (non-timer) event, at most kFramesAfterEvent each. The internal
        // events (the tick, the wake-up) change nothing on screen in this mode: no provider and
        // no log are attached.
        const auto allowed =
            idle.baselineFrames +
            static_cast<std::uint64_t>(kFramesAfterEvent * idle.externalEventsAfterBaseline);
        const bool idleOk = idle.baselineSet && frames <= allowed;
        std::printf("%s baseline_frames=%llu final_frames=%llu external_events=%d seconds=%d%s%s\n",
                    idleOk ? "CURIA_IDLE_OK" : "CURIA_IDLE_FAIL",
                    static_cast<unsigned long long>(idle.baselineFrames),
                    static_cast<unsigned long long>(frames), idle.externalEventsAfterBaseline,
                    idleSeconds,
                    idleOk ? "" : " events:", idleOk ? "" : idle.lastEventTypes.c_str());
        if (!idleOk) {
            exitCode = 1;
        }
    }
    return exitCode;
}

// The chat window (and --idle-test): the controller, the provider, the log watcher and the loop.
int runWindow(const CliOptions& options, WindowSetup& setup, SDL_Window* window,
              ui::IRenderBackend& backend, const ui::FontSet& fonts, Uint32 wakeEvent,
              Uint32 tickEvent) {
    const bool idleMode = options.idleSeconds > 0;
    ui::SdlWake wake(wakeEvent);
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
                       {{"configured", controller->configured()},
                        {"log_found", !setup.logPath.path.empty()},
                        {"log_from_flag", setup.logPath.fromFlag}});
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
    if (!idleMode) {
        chat->focusInput();
    }

    const SDL_TimerID timer = SDL_AddTimer(kTickMs, tickCallback, tickUserdata(tickEvent));
    int exitCode = 0;
    if (timer == 0) {
        std::fprintf(stderr, "%sSDL_AddTimer failed: %s\n", idleMode ? "CURIA_IDLE_FAIL " : "",
                     SDL_GetError());
        exitCode = 1;
    } else {
        exitCode = runEventLoop(window, backend, *controller, *chat, wakeEvent, tickEvent,
                                options.idleSeconds);
        SDL_RemoveTimer(timer);
    }

    // Shut down in order: nothing reads the log any more, the request in flight is stopped,
    // no event is pushed any more, then the controller (and with it the provider, whose
    // destructor joins its net thread) goes.
    if (watcher != nullptr) {
        watcher->stop();
    }
    controller->onStop();
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

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (options.smoke) {
        // CI machines may have no GPU; the software renderer exercises the same code path.
        // SDL_HINT_DEFAULT keeps a user-provided SDL_RENDER_DRIVER environment variable in charge.
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_DEFAULT);
    }

    const int width = options.windowWidth > 0 ? options.windowWidth : kDefaultWidth;
    const int height = options.windowHeight > 0 ? options.windowHeight : kDefaultHeight;
    // A new window opens on the primary display, so its content scale gives the size. TODO: follow
    // SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED (a window moved to a display with another scale).
    float sizeScale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (sizeScale <= 0.0F) {
        sizeScale = 1.0F;
    }
    SDL_Window* window =
        SDL_CreateWindow("Curia", toWindowUnits(width, sizeScale), toWindowUnits(height, sizeScale),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, toWindowUnits(std::min(kMinWidth, width), sizeScale),
                             toWindowUnits(std::min(kMinHeight, height), sizeScale));

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
    ui::applyTheme(uiScale);

    auto backend = ui::createSdlRendererBackend();
    if (!backend->init(window)) {
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    const ui::Rgb clear = ui::palette::kParchment;
    backend->setClearColor(clear.r, clear.g, clear.b, 255);

    int exitCode = 0;
    // Two event types of our own: the mailbox's wake-up and the tick.
    const Uint32 firstEvent = SDL_RegisterEvents(2);
    if (firstEvent == 0) {
        std::fprintf(stderr, "%sSDL_RegisterEvents failed: %s\n",
                     options.idleSeconds > 0 ? "CURIA_IDLE_FAIL " : "", SDL_GetError());
        exitCode = 1;
    } else if (options.smoke) {
        ui::SmokeContext smoke;
        smoke.window = window;
        smoke.backend = backend.get();
        smoke.fonts = fonts;
        smoke.uiScale = uiScale;
        smoke.wakeEvent = firstEvent;
        smoke.screenshotPath = options.screenshotPath;
        exitCode = ui::runSmoke(smoke);
    } else {
        exitCode = runWindow(options, setup, window, *backend, fonts, firstEvent, firstEvent + 1);
    }

    backend->shutdown();
    backend.reset();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}

}  // namespace curia
