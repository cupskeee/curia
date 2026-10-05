// SPDX-License-Identifier: MIT
#include "app.h"

#include <SDL3/SDL.h>
#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "core/build_info.h"
#include "ui/render_backend.h"

namespace curia {

namespace {

constexpr int kFramesAfterEvent = 2;
constexpr int kQuietTicksBeforeBaseline = 2;
constexpr int kMaxTicksToSettle = 30;

// State of the --idle-test mode (all of it is touched on the main thread only; the timer thread
// only pushes events).
struct IdleTest {
    Uint32 tickEventType = 0;
    SDL_TimerID timer = 0;
    int ticks = 0;
    int quietTicks = 0;
    bool eventSinceTick = false;
    bool baselineSet = false;
    std::uint64_t baselineFrames = 0;
    int ticksAfterBaseline = 0;
    int externalEventsAfterBaseline = 0;
    std::string lastEventTypes;  // diagnostics, printed on failure
};

Uint32 SDLCALL idleTickCallback(void* userdata, SDL_TimerID /*timerId*/, Uint32 interval) {
    const auto* test = static_cast<const IdleTest*>(userdata);
    SDL_Event event;
    SDL_zero(event);
    event.type = test->tickEventType;
    SDL_PushEvent(&event);
    return interval;
}

}  // namespace

int runApp(const CliOptions& options) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (options.smoke) {
        // CI machines may have no GPU; the software renderer exercises the same code path.
        // SDL_HINT_DEFAULT keeps a user-provided SDL_RENDER_DRIVER environment variable in charge.
        SDL_SetHintWithPriority(SDL_HINT_RENDER_DRIVER, "software", SDL_HINT_DEFAULT);
    }

    SDL_Window* window =
        SDL_CreateWindow("Curia", 640, 420, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (window == nullptr) {
        std::fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;  // no imgui.ini next to the binary
    ImGui::StyleColorsDark();
    // The content scale only (the DPI scale on Windows and Linux, 1.0 on macOS). The pixel density
    // is already applied through the framebuffer scale by the renderer backend, so using
    // SDL_GetWindowDisplayScale (density times content scale) here would draw the UI twice as
    // large on a Retina Mac.
    float uiScale = SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window));
    if (uiScale <= 0.0F) {
        uiScale = 1.0F;
    }
    ImGui::GetStyle().ScaleAllSizes(uiScale);
    ImGui::GetStyle().FontScaleDpi = uiScale;

    auto backend = ui::createSdlRendererBackend();
    if (!backend->init(window)) {
        ImGui::DestroyContext();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    std::uint64_t frames = 0;
    bool running = true;
    const std::string version(versionString());

    auto drawFrame = [&]() {
        backend->beginFrame();
        ImGui::SetNextWindowPos(ImVec2(24.0F, 24.0F), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(360.0F, 140.0F), ImGuiCond_FirstUseEver);
        ImGui::Begin("Curia");
        ImGui::Text("Curia %s (skeleton)", version.c_str());
        ImGui::Text("Frames drawn: %llu", static_cast<unsigned long long>(frames));
        if (ImGui::Button("Quit")) {
            running = false;
        }
        ImGui::End();
        backend->endFrame();
        ++frames;
    };

    int exitCode = 0;

    if (options.smoke) {
        for (int i = 0; i < 3; ++i) {
            drawFrame();
        }
        std::printf("CURIA_SMOKE_OK frames=%llu renderer=%s ui_scale=%.2f\n",
                    static_cast<unsigned long long>(frames), backend->rendererName().c_str(),
                    static_cast<double>(uiScale));
    } else {
        IdleTest idle;
        const bool idleMode = options.idleSeconds > 0;
        if (idleMode) {
            idle.tickEventType = SDL_RegisterEvents(1);
            if (idle.tickEventType == 0) {
                std::fprintf(stderr, "CURIA_IDLE_FAIL SDL_RegisterEvents failed: %s\n",
                             SDL_GetError());
                exitCode = 1;
                running = false;
            } else {
                idle.timer = SDL_AddTimer(1000, idleTickCallback, &idle);
                if (idle.timer == 0) {
                    std::fprintf(stderr, "CURIA_IDLE_FAIL SDL_AddTimer failed: %s\n",
                                 SDL_GetError());
                    exitCode = 1;
                    running = false;
                }
            }
        }

        // Event-driven loop: draw two frames after every real event (enough for ImGui to settle
        // hover and layout state), then block in SDL_WaitEvent until something happens. No polling.
        // Known limit, to be revisited with the chat UI (M4): ImGui features that need time-driven
        // frames (text-caret blink, held-button repeat, tooltip delay) will need
        // SDL_WaitEventTimeout while such an item is active.
        int redraw = kFramesAfterEvent;
        while (running) {
            SDL_Event event;
            bool have = false;
            if (redraw > 0) {
                have = SDL_PollEvent(&event);
            } else if (!SDL_WaitEvent(&event)) {
                std::fprintf(stderr, "SDL_WaitEvent failed: %s\n", SDL_GetError());
                exitCode = 1;
                break;
            } else {
                have = true;
            }
            while (have) {
                if (idleMode && event.type == idle.tickEventType) {
                    // Test-only timer tick: must not cause a redraw.
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
                    } else if (++idle.ticksAfterBaseline >= options.idleSeconds) {
                        running = false;
                    }
                    idle.eventSinceTick = false;
                } else {
                    backend->processEvent(event);
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
            if (running && redraw > 0) {
                drawFrame();
                --redraw;
            }
        }

        if (idle.timer != 0) {
            SDL_RemoveTimer(idle.timer);
        }
        if (idleMode && exitCode == 0) {
            // Frames are only ever drawn after an event, so every frame after the baseline must be
            // explained by a real (non-timer) event, at most kFramesAfterEvent each.
            const auto allowed =
                idle.baselineFrames +
                static_cast<std::uint64_t>(kFramesAfterEvent * idle.externalEventsAfterBaseline);
            const bool idleOk = idle.baselineSet && frames <= allowed;
            std::printf(
                "%s baseline_frames=%llu final_frames=%llu external_events=%d seconds=%d%s%s\n",
                idleOk ? "CURIA_IDLE_OK" : "CURIA_IDLE_FAIL",
                static_cast<unsigned long long>(idle.baselineFrames),
                static_cast<unsigned long long>(frames), idle.externalEventsAfterBaseline,
                options.idleSeconds,
                idleOk ? "" : " events:", idleOk ? "" : idle.lastEventTypes.c_str());
            if (!idleOk) {
                exitCode = 1;
            }
        }
    }

    backend->shutdown();
    backend.reset();
    ImGui::DestroyContext();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return exitCode;
}

}  // namespace curia
