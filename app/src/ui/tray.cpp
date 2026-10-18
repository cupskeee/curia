// SPDX-License-Identifier: MIT
#include "ui/tray.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

#include "chatview/view_text.h"
#include "ui/theme.h"

// While a tray exists SDL wakes SDL_WaitEvent every 50 ms on Unix-like systems other than macOS, to
// poll it (SDL 3.4.18, SDL_events.c, TRAY_POLL_INTERVAL_NS), also while the window is minimised:
// there is no tray there in M4 (NFR-1). The in-game button opens the window, its close button
// quits.
#if defined(SDL_PLATFORM_UNIX) && !defined(SDL_PLATFORM_APPLE)
#define CURIA_TRAY_POLLED_BY_SDL 1
#else
#define CURIA_TRAY_POLLED_BY_SDL 0
#endif

namespace curia::ui {

#if !CURIA_TRAY_POLLED_BY_SDL
namespace {

// The icon's side in pixels (SDL draws it into a 22-point image on macOS: 44 pixels on a Retina
// menu bar).
constexpr int kIconSide = 44;
// Samples per pixel along each axis (smooth edges).
constexpr int kSamples = 4;

// A deep red disc with a light rim and a light "C": the disc stands out on a light menu bar, the
// rim and the letter on a dark one. Drawn here, so there is no image file to ship.
SDL_Surface* drawIcon() {
    SDL_Surface* icon = SDL_CreateSurface(kIconSide, kIconSide, SDL_PIXELFORMAT_RGBA32);
    if (icon == nullptr || !SDL_LockSurface(icon)) {
        if (icon != nullptr) {
            SDL_DestroySurface(icon);
        }
        return nullptr;
    }
    const Rgb disc = palette::kAccent;
    const Rgb light = palette::kParchment;
    const double centre = kIconSide / 2.0;
    const double discRadius = 19.5;
    const double rimRadius = 21.5;
    const double letterInner = 7.0;
    const double letterOuter = 12.5;
    auto* pixels = static_cast<std::uint8_t*>(icon->pixels);
    for (int y = 0; y < kIconSide; ++y) {
        for (int x = 0; x < kIconSide; ++x) {
            double r = 0;
            double g = 0;
            double b = 0;
            int covered = 0;
            for (int sy = 0; sy < kSamples; ++sy) {
                for (int sx = 0; sx < kSamples; ++sx) {
                    const double dx = x + (sx + 0.5) / kSamples - centre;
                    const double dy = y + (sy + 0.5) / kSamples - centre;
                    const double d = std::sqrt(dx * dx + dy * dy);
                    const Rgb* colour = nullptr;
                    if (d <= discRadius) {
                        // The letter: a ring open towards the right (a quarter of a turn).
                        const bool open = dx > 0 && std::fabs(dy) < dx;
                        colour = (d >= letterInner && d <= letterOuter && !open) ? &light : &disc;
                    } else if (d <= rimRadius) {
                        colour = &light;
                    }
                    if (colour != nullptr) {
                        r += colour->r;
                        g += colour->g;
                        b += colour->b;
                        ++covered;
                    }
                }
            }
            std::uint8_t* pixel = pixels + (static_cast<std::ptrdiff_t>(y) * icon->pitch) + (x * 4);
            const auto channel = [covered](double sum) {
                return static_cast<std::uint8_t>(covered > 0 ? std::lround(sum / covered) : 0);
            };
            pixel[0] = channel(r);
            pixel[1] = channel(g);
            pixel[2] = channel(b);
            pixel[3] =
                static_cast<std::uint8_t>(std::lround(255.0 * covered / (kSamples * kSamples)));
        }
    }
    SDL_UnlockSurface(icon);
    return icon;
}

// The event type and the entry travel in the callback's userdata (no storage to outlive).
void* entryUserdata(Uint32 eventType, Tray::Entry entry) {
    return reinterpret_cast<void*>((static_cast<std::uintptr_t>(eventType) << 2U) |
                                   static_cast<std::uintptr_t>(entry));
}

void SDLCALL onEntry(void* userdata, SDL_TrayEntry* /*entry*/) {
    const auto packed = reinterpret_cast<std::uintptr_t>(userdata);
    SDL_Event event;
    SDL_zero(event);
    event.type = static_cast<Uint32>(packed >> 2U);
    event.user.code = static_cast<Sint32>(packed & 3U);
    SDL_PushEvent(&event);
}

}  // namespace
#endif

std::unique_ptr<Tray> Tray::create(Uint32 eventType, std::string_view& status) {
#if CURIA_TRAY_POLLED_BY_SDL
    (void)eventType;
    status = "skipped_linux";
    return nullptr;
#else
    SDL_Surface* icon = drawIcon();
    SDL_Tray* tray = SDL_CreateTray(icon, std::string(chatview::text::kTrayTooltip).c_str());
    if (icon != nullptr) {
        SDL_DestroySurface(icon);
    }
    if (tray == nullptr) {
        status = "unavailable";
        return nullptr;
    }
    SDL_TrayMenu* menu = SDL_CreateTrayMenu(tray);
    if (menu != nullptr) {
        const struct {
            std::string_view label;
            Entry entry;
        } entries[] = {{chatview::text::kTrayShow, Show},
                       {chatview::text::kTrayHide, Hide},
                       {chatview::text::kTrayQuit, Quit}};
        for (const auto& item : entries) {
            SDL_TrayEntry* entry = SDL_InsertTrayEntryAt(menu, -1, std::string(item.label).c_str(),
                                                         SDL_TRAYENTRY_BUTTON);
            if (entry != nullptr) {
                SDL_SetTrayEntryCallback(entry, onEntry, entryUserdata(eventType, item.entry));
            }
        }
    }
    status = "created";
    return std::unique_ptr<Tray>(new Tray(tray));
#endif
}

Tray::~Tray() {
    if (tray_ != nullptr) {
        SDL_DestroyTray(tray_);
    }
}

}  // namespace curia::ui
