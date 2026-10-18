// SPDX-License-Identifier: MIT
// The macOS platform layer (Objective-C++, ARC). It asks NSWorkspace and CoreGraphics on demand and
// installs no observer. Nothing here calls an Accessibility API, asks for a permission or reads a
// window title (docs/research-notes.md section 2.5).
//
// NSWorkspace's list of running applications and its frontmost application change only when the
// main run loop runs in a common mode (the SDK's NSRunningApplication.h). In the app SDL runs it
// while it waits for events (SDL_WaitEvent too), so the once-a-second tick sees a fresh list; a
// process that never runs it (the unit tests) keeps seeing the list of its first call.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <string>

#include "platform/env.h"
#include "platform/game_window.h"
#include "platform/platform.h"

namespace curia::platform {

namespace {

bool isGameApplication(NSRunningApplication* app) {
    NSString* name = app.executableURL.lastPathComponent;
    return name != nil && isGameProcessName(std::string(name.UTF8String));
}

std::vector<long long> gamePids() {
    std::vector<long long> pids;
    for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
        if (isGameApplication(app)) {
            pids.push_back(static_cast<long long>(app.processIdentifier));
        }
    }
    return pids;
}

// One entry of CGWindowListCopyWindowInfo: owner, layer and bounds only.
std::optional<WindowCandidate> candidateFrom(NSDictionary* window) {
    NSNumber* pid = window[(__bridge NSString*)kCGWindowOwnerPID];
    NSNumber* layer = window[(__bridge NSString*)kCGWindowLayer];
    NSDictionary* bounds = window[(__bridge NSString*)kCGWindowBounds];
    if (pid == nil || layer == nil || bounds == nil) {
        return std::nullopt;
    }
    CGRect rect = CGRectZero;
    if (!CGRectMakeWithDictionaryRepresentation((__bridge CFDictionaryRef)bounds, &rect)) {
        return std::nullopt;
    }
    WindowCandidate candidate;
    candidate.pid = pid.longLongValue;
    candidate.layer = layer.intValue;
    // CoreGraphics counts from the top-left corner of the primary display, y down: the same
    // coordinates as ScreenRect.
    candidate.rect = ScreenRect{rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
    return candidate;
}

class MacPlatform final : public Platform {
public:
    std::string_view name() const override { return "macos"; }

    long long processId() const override { return static_cast<long long>(getpid()); }

    std::vector<std::filesystem::path> candidateCk3UserDirs() const override {
        const std::string home = getEnv("HOME");
        if (home.empty()) {
            return {};
        }
        return {std::filesystem::path(home) / "Documents" / "Paradox Interactive" /
                "Crusader Kings III"};
    }

    std::optional<bool> isGameRunning() const override {
        @autoreleasepool {
            return !gamePids().empty();
        }
    }

    std::optional<bool> gameIsFrontmost() const override {
        @autoreleasepool {
            NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
            return front != nil && isGameApplication(front);
        }
    }

    std::optional<ScreenRect> findGameWindow() const override {
        @autoreleasepool {
            const std::vector<long long> pids = gamePids();
            if (pids.empty()) {
                return std::nullopt;
            }
            NSArray* list = CFBridgingRelease(CGWindowListCopyWindowInfo(
                kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID));
            if (list == nil) {
                return std::nullopt;
            }
            std::vector<WindowCandidate> windows;
            for (NSDictionary* window in list) {
                const std::optional<WindowCandidate> candidate = candidateFrom(window);
                if (candidate.has_value()) {
                    windows.push_back(*candidate);
                }
            }
            return pickGameWindow(windows, pids);
        }
    }
};

}  // namespace

std::unique_ptr<Platform> create() {
    return std::make_unique<MacPlatform>();
}

}  // namespace curia::platform
