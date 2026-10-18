// SPDX-License-Identifier: MIT
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

#include "core/app_log.h"
#include "platform/platform.h"
#include "ui/overlay.h"

namespace curia::ui {

namespace {

// Window sizes in the units SDL_CreateWindow takes. macOS and Wayland (by default) count in points
// and report a content scale of 1; Windows and X11 count in pixels and report the DPI scale, so a
// size in logical pixels is multiplied by it there.
int toWindowUnits(int logical, float contentScale) {
    return static_cast<int>(std::lround(static_cast<float>(logical) * contentScale));
}

// The PR 7 chat window: normal, resizable, on top of other windows in the chat window (the
// companion window of FR-OVL-8). Shown means restored and raised; hidden means minimised.
class PlainOverlay final : public Overlay {
public:
    explicit PlainOverlay(const OverlayOptions& options)
        : log_(options.log), platform_(options.platform) {
        // A new window opens on the primary display, so its content scale gives the size. TODO:
        // follow SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED (a window moved to a display with another
        // scale).
        float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
        if (scale <= 0.0F) {
            scale = 1.0F;
        }
        SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
        if (options.alwaysOnTop) {
            flags |= SDL_WINDOW_ALWAYS_ON_TOP;
        }
        window_ = SDL_CreateWindow("Curia", toWindowUnits(options.width, scale),
                                   toWindowUnits(options.height, scale), flags);
        if (window_ != nullptr) {
            SDL_SetWindowMinimumSize(
                window_, toWindowUnits(std::min(options.minWidth, options.width), scale),
                toWindowUnits(std::min(options.minHeight, options.height), scale));
        }
    }

    ~PlainOverlay() override {
        if (window_ != nullptr) {
            SDL_DestroyWindow(window_);
        }
    }

    PlainOverlay(const PlainOverlay&) = delete;
    PlainOverlay& operator=(const PlainOverlay&) = delete;

    SDL_Window* window() const override { return window_; }

    void show(ShowReason reason) override {
        // Only a minimised window is restored (SDL_RestoreWindow would also shrink a maximised
        // one). On macOS a normal window that is raised activates Curia, which leaves CK3's
        // Fullscreen Space: the reason the panel exists.
        if ((SDL_GetWindowFlags(window_) & SDL_WINDOW_MINIMIZED) != 0) {
            SDL_RestoreWindow(window_);
        }
        SDL_RaiseWindow(window_);
        if (log_ != nullptr) {
            log_->log(AppLog::Level::Info, "overlay_shown",
                      {{"kind", kind()}, {"reason", toString(reason)}});
        }
    }

    void hide() override {
        SDL_MinimizeWindow(window_);
        if (log_ != nullptr) {
            log_->log(AppLog::Level::Info, "overlay_hidden", {{"kind", kind()}});
        }
    }

    // The OS hands the keys on when a window is minimised; only the focus is logged.
    bool settle() override {
        logFocus("settled");
        return false;
    }

    void logFocus(const char* why) const override {
        if (log_ == nullptr) {
            return;
        }
        const std::optional<bool> gameFront =
            platform_ != nullptr ? platform_->gameIsFrontmost() : std::nullopt;
        log_->log(AppLog::Level::Info, "overlay_focus",
                  {{"why", why},
                   {"kind", kind()},
                   {"visible", visible()},
                   {"input_focus", (SDL_GetWindowFlags(window_) & SDL_WINDOW_INPUT_FOCUS) != 0},
                   {"ck3_front", gameFront.has_value() ? (gameFront.value_or(false) ? "yes" : "no")
                                                       : "unknown"}});
    }

    bool visible() const override {
        return (SDL_GetWindowFlags(window_) & (SDL_WINDOW_HIDDEN | SDL_WINDOW_MINIMIZED)) == 0;
    }

    const char* kind() const override { return "window"; }

private:
    SDL_Window* window_ = nullptr;
    AppLog* log_ = nullptr;
    const platform::Platform* platform_ = nullptr;
};

}  // namespace

const char* toString(ShowReason reason) noexcept {
    switch (reason) {
    case ShowReason::Start:
        return "start";
    case ShowReason::GameClick:
        return "game_click";
    case ShowReason::Hotkey:
        return "hotkey";
    case ShowReason::Tray:
        return "tray";
    }
    return "start";
}

void prepareOverlayBeforeInit(WindowKind kind) {
    if (kind != WindowKind::Overlay) {
        return;
    }
    // Without these SDL applies the Regular activation policy at SDL_Init (a Dock icon, and an
    // app that activates) and activates Curia whenever a window is shown or raised, which would
    // pull CK3 out of its Fullscreen Space (M2).
    SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_RAISED, "0");
}

std::unique_ptr<Overlay> createPlainOverlay(const OverlayOptions& options) {
    auto overlay = std::make_unique<PlainOverlay>(options);
    if (overlay->window() == nullptr) {
        return nullptr;
    }
    return overlay;
}

std::unique_ptr<Overlay> createOverlay(WindowKind kind, const OverlayOptions& options) {
    return kind == WindowKind::Overlay ? createPanelOverlay(options) : createPlainOverlay(options);
}

#if !defined(__APPLE__)
// The panel is macOS only (the command line refuses --window overlay elsewhere).
std::unique_ptr<Overlay> createPanelOverlay(const OverlayOptions& /*options*/) {
    SDL_SetError("the overlay panel exists on macOS only");
    return nullptr;
}
#endif

}  // namespace curia::ui
