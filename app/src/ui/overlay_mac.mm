// SPDX-License-Identifier: MIT
// The overlay panel on macOS (Objective-C++, ARC): the recipe M2 found visible over CK3's
// Fullscreen Space, ported from the probe (app/probe/probe_mac.mm, combination 1;
// docs/spikes/m2-results.md). Nothing here calls an Accessibility API, plays a sound, installs an
// event tap or a global event monitor, or asks for a permission (docs/research-notes.md 2.5).
#import <AppKit/AppKit.h>
#include <SDL3/SDL.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/app_log.h"
#include "platform/game_window.h"
#include "platform/placement.h"
#include "platform/platform.h"
#include "ui/overlay.h"
#include "ui/theme.h"

// The panel's content view. SDL keeps the content view of a wrapped window (it adds its Metal view
// inside it), and a plain NSView refuses the click that would make a window of an inactive app key.
@interface CuriaPanelView : NSView
@end

@implementation CuriaPanelView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
@end

// Borderless and non-activating, but able to become the key window so that typing works (a
// borderless window cannot become key by default); never the main window.
@interface CuriaPanel : NSPanel
@end

@implementation CuriaPanel
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

namespace curia::ui {

namespace {

// The gap between the panel and the edges of CK3's window (or of the display), in points.
constexpr double kMarginPoints = 24.0;

NSColor* colour(Rgb rgb) {
    return [NSColor colorWithSRGBRed:rgb.r / 255.0
                               green:rgb.g / 255.0
                                blue:rgb.b / 255.0
                               alpha:1.0];
}

const char* policyName(NSApplicationActivationPolicy policy) {
    switch (policy) {
    case NSApplicationActivationPolicyRegular:
        return "regular";
    case NSApplicationActivationPolicyAccessory:
        return "accessory";
    case NSApplicationActivationPolicyProhibited:
        return "prohibited";
    }
    return "unknown";
}

class PanelOverlay final : public Overlay {
public:
    explicit PanelOverlay(const OverlayOptions& options) : options_(options) {
        @autoreleasepool {
            NSApplication* app = NSApplication.sharedApplication;
            const char* before = policyName(app.activationPolicy);
            // Explicitly: in M2 SDL_HINT_MAC_BACKGROUND_APP alone left the Regular policy (here
            // the hint gives Prohibited, and Curia.app's LSUIElement gives Accessory already, in
            // which case the call reports no switch).
            const bool switched =
                [app setActivationPolicy:NSApplicationActivationPolicyAccessory] == YES;
            log("overlay_policy", {{"before", before},
                                   {"switched", switched},
                                   {"policy", policyName(app.activationPolicy)}});

            const NSRect rect = NSMakeRect(0, 0, options.width, options.height);
            CuriaPanel* panel =
                [[CuriaPanel alloc] initWithContentRect:rect
                                              styleMask:NSWindowStyleMaskBorderless |
                                                        NSWindowStyleMaskNonactivatingPanel
                                                backing:NSBackingStoreBuffered
                                                  defer:NO];
            panel.contentView = [[CuriaPanelView alloc] initWithFrame:rect];
            panel.hidesOnDeactivate = NO;  // NSPanel's default would hide it whenever CK3 is active
            panel.floatingPanel = YES;
            panel.becomesKeyOnlyIfNeeded = NO;
            panel.releasedWhenClosed = NO;
            panel.acceptsMouseMovedEvents = YES;
            panel.animationBehavior = NSWindowAnimationBehaviorNone;
            // Opaque parchment until the first frame is drawn (no transparency in M4).
            panel.backgroundColor = colour(palette::kParchment);
            panel.level = NSScreenSaverWindowLevel;
            panel.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                       NSWindowCollectionBehaviorFullScreenAuxiliary |
                                       NSWindowCollectionBehaviorStationary;
            panel_ = panel;

            // SDL wraps the panel: it marks the window SDL_WINDOW_EXTERNAL, applies none of its
            // own flags, keeps a strong reference to the NSWindow while the SDL window lives and
            // neither closes nor hides it when the SDL window is destroyed (SDL 3.4.18,
            // SDL_cocoawindow.m and SDL_video.c). So this object keeps its own reference
            // (panel_), destroys the SDL window first and releases the panel after.
            const SDL_PropertiesID props = SDL_CreateProperties();
            SDL_SetPointerProperty(props, SDL_PROP_WINDOW_CREATE_COCOA_WINDOW_POINTER,
                                   (__bridge void*)panel);
            SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_HIGH_PIXEL_DENSITY_BOOLEAN, true);
            SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING, "Curia");
            window_ = SDL_CreateWindowWithProperties(props);
            SDL_DestroyProperties(props);
            // Never SDL_SetWindowAlwaysOnTop, SDL_SetWindowResizable or SDL_SetWindowFullscreen
            // on it: SDL writes its own level and collection behaviour there.
        }
    }

    ~PanelOverlay() override {
        @autoreleasepool {
            [panel_ orderOut:nil];
            if (window_ != nullptr) {
                SDL_DestroyWindow(window_);
                window_ = nullptr;
            }
            remembered_ = nil;
            panel_ = nil;  // the last reference: the panel goes now
        }
    }

    PanelOverlay(const PanelOverlay&) = delete;
    PanelOverlay& operator=(const PanelOverlay&) = delete;

    SDL_Window* window() const override { return window_; }

    void show(ShowReason reason) override {
        @autoreleasepool {
            NSApplication* app = NSApplication.sharedApplication;
            // The last fallback for giving the keys back on hide: the app in front now, unless
            // that is us (settle() asks the app in front at that time first).
            NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
            if (front != nil && front.processIdentifier != getpid()) {
                remembered_ = front;
            }
            // The start-up show asks for no keys, but the launch (or the tray created just before)
            // can make Curia active and the panel key anyway (research-notes 2.17): settle() then
            // hands the keys back as after a hide.
            keyCheck_ = reason == ShowReason::Start ? KeyCheck::AfterStart : KeyCheck::None;
            settleWhy_ = "settled_show";
            place();                        // on every show (FR-OVL-9)
            [panel_ orderFrontRegardless];  // in front without activating Curia
            // At start-up the player has not asked to type: never take the keys then (the launch
            // itself may still make Curia active, docs/architecture.md section 5.2).
            const OverlayFocus focus =
                reason == ShowReason::Start ? OverlayFocus::Click : options_.focus;
            switch (focus) {
            case OverlayFocus::Key:
                [panel_ makeKeyWindow];  // the keys without activating Curia (the E6 question)
                break;
            case OverlayFocus::Click:
                break;  // M2's verified behaviour: the first click on the panel takes the keys
            case OverlayFocus::Activate:
                [panel_ makeKeyWindow];
                // The escalation. The forceful call, not the cooperative -activate, which may not
                // activate the app at all while CK3 is the active app (NSApplication.h); M2 saw
                // this call (in SDL's tray code) make the probe active at once with CK3 in front,
                // without pulling CK3 out of its Fullscreen Space (E11). The SDK marks the call as
                // to be deprecated; the pragma keeps a later SDK's warning from failing the build.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [app activateIgnoringOtherApps:YES];
#pragma clang diagnostic pop
                break;
            }
            log("overlay_shown", {{"kind", kind()},
                                  {"reason", toString(reason)},
                                  {"focus", toString(focus)},
                                  {"panel_key", panel_.isKeyWindow == YES},
                                  {"app_active", app.isActive == YES},
                                  {"ck3_front", gameFrontWord()}});
        }
    }

    void hide() override {
        @autoreleasepool {
            [panel_ orderOut:nil];
            // In M2 orderOut alone gave the keys back to CK3 at once; settle() checks a moment
            // later and helps only if Curia is still the active app then.
            keyCheck_ = KeyCheck::AfterHide;
            settleWhy_ = "settled_hide";
            log("overlay_hidden",
                {{"kind", kind()},
                 {"app_active", NSApplication.sharedApplication.isActive == YES}});
        }
    }

    bool settle() override {
        @autoreleasepool {
            const bool check = (keyCheck_ == KeyCheck::AfterHide && !visible()) ||
                               (keyCheck_ == KeyCheck::AfterStart && visible());
            const char* after = keyCheck_ == KeyCheck::AfterStart ? "start" : "hide";
            keyCheck_ = KeyCheck::None;
            if (check && returnKeys(after)) {
                settleWhy_ = "after_return";
                return true;
            }
            logFocus(settleWhy_);
            return false;
        }
    }

    void logFocus(const char* why) const override {
        if (options_.log == nullptr) {
            return;
        }
        @autoreleasepool {
            log("overlay_focus", {{"why", why},
                                  {"kind", kind()},
                                  {"visible", visible()},
                                  {"panel_key", panel_.isKeyWindow == YES},
                                  {"app_active", NSApplication.sharedApplication.isActive == YES},
                                  {"ck3_front", gameFrontWord()}});
        }
    }

    bool visible() const override { return panel_.isVisible == YES; }

    const char* kind() const override { return "panel"; }

private:
    void log(std::string_view event, std::initializer_list<AppLog::Field> fields) const {
        if (options_.log != nullptr) {
            options_.log->log(AppLog::Level::Info, event, fields);
        }
    }

    // Is CK3 in front (NSWorkspace, the design choice of M2), as a word for the app log.
    const char* gameFrontWord() const {
        const std::optional<bool> gameFront =
            options_.platform != nullptr ? options_.platform->gameIsFrontmost() : std::nullopt;
        if (!gameFront.has_value()) {
            return "unknown";
        }
        return gameFront.value_or(false) ? "yes" : "no";
    }

    // After a hide or the start-up show (`after`), if Curia is the active app: asks another app to
    // activate so that the keys go back to the game (FR-OVL-2) or to the app Curia was started
    // from. Which app is worked out now, not at the last show (the
    // panel stays up while the player moves between apps): the app in front by NSWorkspace if it
    // is not Curia (in M2 it stayed CK3 while the panel was key and Curia active), else a running
    // CK3, else the app remembered at the last show. Cooperative activation first (yield, then
    // ask that app to activate, macOS 14 and later), then the plain request: a request, not a
    // guarantee. Logs overlay_keys with the choice as a word. True if an app was asked.
    bool returnKeys(const char* after) {
        NSApplication* app = NSApplication.sharedApplication;
        if (app.isActive == NO) {
            // After a hide M2's path: orderOut was enough.
            log("overlay_keys", {{"after", after}, {"returned", "not_needed"}});
            return false;
        }
        NSRunningApplication* target = nil;
        const char* via = "none";
        NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
        NSRunningApplication* game = nil;
        if (front != nil && front.processIdentifier != getpid()) {
            target = front;
            via = "frontmost";
        } else if ((game = runningGame()) != nil) {
            target = game;
            via = "game";
        } else if (remembered_ != nil && remembered_.terminated == NO) {
            target = remembered_;
            via = "remembered";
        }
        if (target == nil) {
            log("overlay_keys", {{"after", after}, {"returned", via}});
            return false;
        }
        [app yieldActivationToApplication:target];
        bool accepted = [target activateFromApplication:NSRunningApplication.currentApplication
                                                options:0] == YES;
        if (!accepted) {
            accepted = [target activateWithOptions:NSApplicationActivateAllWindows] == YES;
        }
        log("overlay_keys", {{"after", after},
                             {"returned", via},
                             {"to", isGame(target) ? "ck3" : "other"},
                             {"accepted", accepted}});
        return true;
    }

    static bool isGame(NSRunningApplication* app) {
        NSString* name = app.executableURL.lastPathComponent;
        return name != nil && platform::isGameProcessName(std::string(name.UTF8String));
    }

    static NSRunningApplication* runningGame() {
        for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
            if (app.terminated == NO && isGame(app)) {
                return app;
            }
        }
        return nil;
    }

    // FR-OVL-9: on the display with the largest part of CK3's window, at its right edge; without a
    // CK3 window on the pointer's display. The displays come from NSScreen (bottom-left origin)
    // and CK3's window from CoreGraphics (top-left origin): platform/placement.h converts.
    void place() {
        NSArray<NSScreen*>* screens = NSScreen.screens;
        if (screens.count == 0) {
            return;
        }
        // NSScreen.screens[0] is the display with the menu bar, whose frame starts at 0,0: the
        // origin of both coordinate systems.
        const double primaryHeight = screens[0].frame.size.height;
        platform::PlacementRequest request;
        for (NSScreen* screen in screens) {
            const NSRect frame = screen.frame;
            const NSRect visibleFrame = screen.visibleFrame;
            request.displays.push_back(platform::DisplayArea{
                platform::cocoaToTopLeft(platform::ScreenRect{frame.origin.x, frame.origin.y,
                                                              frame.size.width, frame.size.height},
                                         primaryHeight),
                platform::cocoaToTopLeft(
                    platform::ScreenRect{visibleFrame.origin.x, visibleFrame.origin.y,
                                         visibleFrame.size.width, visibleFrame.size.height},
                    primaryHeight)});
        }
        if (options_.followGame && options_.platform != nullptr) {
            request.gameWindow = options_.platform->findGameWindow();
        }
        const NSPoint pointer = NSEvent.mouseLocation;
        request.pointer =
            platform::cocoaToTopLeft(platform::ScreenPoint{pointer.x, pointer.y}, primaryHeight);
        request.width = options_.width;
        request.height = options_.height;
        request.minWidth = options_.minWidth;
        request.minHeight = options_.minHeight;
        request.margin = kMarginPoints;
        const std::optional<platform::Placement> placed = platform::placeOverlay(request);
        if (!placed.has_value()) {
            return;
        }
        const platform::Placement& where = *placed;
        const platform::ScreenRect cocoa = platform::topLeftToCocoa(where.frame, primaryHeight);
        [panel_ setFrame:NSMakeRect(cocoa.x, cocoa.y, cocoa.width, cocoa.height) display:NO];
        log("overlay_placed", {{"basis", platform::toString(where.basis)},
                               {"display", where.display},
                               {"displays", request.displays.size()},
                               {"game_window", request.gameWindow.has_value()},
                               {"x", static_cast<long long>(std::lround(where.frame.x))},
                               {"y", static_cast<long long>(std::lround(where.frame.y))},
                               {"w", static_cast<long long>(std::lround(where.frame.width))},
                               {"h", static_cast<long long>(std::lround(where.frame.height))}});
    }

    OverlayOptions options_;
    CuriaPanel* panel_ = nil;
    SDL_Window* window_ = nullptr;
    NSRunningApplication* remembered_ = nil;
    // Whose keys settle() still has to check: none, the start-up show's or a hide's.
    enum class KeyCheck : std::uint8_t { None, AfterStart, AfterHide };
    KeyCheck keyCheck_ = KeyCheck::None;
    // What the next settled focus line is about.
    const char* settleWhy_ = "settled_show";
};

}  // namespace

std::unique_ptr<Overlay> createPanelOverlay(const OverlayOptions& options) {
    auto overlay = std::make_unique<PanelOverlay>(options);
    if (overlay->window() == nullptr) {
        return nullptr;
    }
    return overlay;
}

}  // namespace curia::ui
