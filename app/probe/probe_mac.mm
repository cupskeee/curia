// SPDX-License-Identifier: MIT
#include "probe_mac.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <Carbon/Carbon.h>
#import <CoreGraphics/CoreGraphics.h>
#include <unistd.h>

#include <memory>

#include "probe_log.h"

// The panel's content view. SDL keeps the content view of a wrapped window (it does not install
// its own SDL3View), and a plain NSView refuses the click that would activate a background window.
@interface CuriaProbeView : NSView
@end

@implementation CuriaProbeView
- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}
@end

// Our panel: borderless and non-activating, but able to become the key window so that typing works
// (a borderless NSWindow cannot become key by default). Whether it receives keys while the app is
// inactive is one of the things the probe measures.
@interface CuriaProbePanel : NSPanel
@end

@implementation CuriaProbePanel
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

namespace curia::probe::mac {

namespace {

std::string str(NSString* text) {
    return text != nil ? std::string(text.UTF8String) : std::string("nil");
}

CuriaProbePanel* panelFrom(void* pointer) {
    return (__bridge CuriaProbePanel*)pointer;
}

std::string policyName(NSApplicationActivationPolicy policy) {
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

NSArray<NSRunningApplication*>* ck3Applications() {
    NSMutableArray<NSRunningApplication*>* found = [NSMutableArray array];
    for (NSRunningApplication* app in NSWorkspace.sharedWorkspace.runningApplications) {
        NSString* name = app.executableURL.lastPathComponent;
        if (name != nil && [name caseInsensitiveCompare:@"ck3"] == NSOrderedSame) {
            [found addObject:app];
        }
    }
    return found;
}

std::string appLabel(NSRunningApplication* app) {
    return fields({kv("name", str(app.localizedName)),
                   kv("exec", str(app.executableURL.lastPathComponent)),
                   kv("pid", static_cast<long long>(app.processIdentifier)),
                   kv("bundle", str(app.bundleIdentifier))});
}

std::string rectText(CGRect rect) {
    char text[96];
    std::snprintf(text, sizeof text, "%.0f,%.0f,%.0f,%.0f", rect.origin.x, rect.origin.y,
                  rect.size.width, rect.size.height);
    return text;
}

std::vector<CGDirectDisplayID> activeDisplays() {
    CGDirectDisplayID ids[16];
    uint32_t count = 0;
    if (CGGetActiveDisplayList(16, ids, &count) != kCGErrorSuccess) {
        return {};
    }
    return std::vector<CGDirectDisplayID>(ids, ids + count);
}

// CGDisplayIsCaptured is deprecated since macOS 10.9 ("No longer supported"), so a 0 does not prove
// that a display is not captured; a 1 would be informative. It is the only query there is.
bool displayCaptured(CGDirectDisplayID id) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    return CGDisplayIsCaptured(id) != 0;
#pragma clang diagnostic pop
}

std::function<void(int)>& hotkeyCallback() {
    static std::function<void(int)> callback;
    return callback;
}

OSStatus hotkeyHandler(EventHandlerCallRef /*next*/, EventRef event, void* /*userData*/) {
    EventHotKeyID id{};
    const OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                              nullptr, sizeof id, nullptr, &id);
    if (status == noErr && hotkeyCallback()) {
        hotkeyCallback()(static_cast<int>(id.id));
    }
    return noErr;
}

NSMutableArray* observerTokens() {
    static NSMutableArray* tokens = [NSMutableArray array];
    return tokens;
}

NSRunningApplication* __strong& remembered() {
    static NSRunningApplication* __strong app = nil;
    return app;
}

}  // namespace

bool setAccessoryPolicy() {
    @autoreleasepool {
        return [NSApplication.sharedApplication
            setActivationPolicy:NSApplicationActivationPolicyAccessory];
    }
}

std::string activationPolicyName() {
    @autoreleasepool {
        return policyName(NSApplication.sharedApplication.activationPolicy);
    }
}

void* createPanel(int width, int height, bool transparent) {
    @autoreleasepool {
        const NSRect rect = NSMakeRect(0, 0, width, height);
        const NSWindowStyleMask style =
            NSWindowStyleMaskBorderless | NSWindowStyleMaskNonactivatingPanel;
        CuriaProbePanel* panel = [[CuriaProbePanel alloc] initWithContentRect:rect
                                                                    styleMask:style
                                                                      backing:NSBackingStoreBuffered
                                                                        defer:NO];
        panel.contentView = [[CuriaProbeView alloc] initWithFrame:rect];
        panel.hidesOnDeactivate = NO;
        panel.floatingPanel = YES;  // as in Apple's sample; the level is set by applyCombo
        panel.becomesKeyOnlyIfNeeded = NO;
        panel.releasedWhenClosed = NO;
        panel.acceptsMouseMovedEvents = YES;
        panel.animationBehavior = NSWindowAnimationBehaviorNone;
        if (transparent) {
            panel.opaque = NO;
            panel.hasShadow = NO;
            panel.backgroundColor = NSColor.clearColor;
        }
        return const_cast<void*>(CFBridgingRetain(panel));
    }
}

void releasePanel(void* panel) {
    if (panel != nullptr) {
        (void)CFBridgingRelease(panel);
    }
}

void applyCombo(void* panel, const Combo& combo) {
    @autoreleasepool {
        NSWindowCollectionBehavior behavior = 0;
        if ((combo.flags & kJoinAllSpaces) != 0U) {
            behavior |= NSWindowCollectionBehaviorCanJoinAllSpaces;
        }
        if ((combo.flags & kFullScreenAuxiliary) != 0U) {
            behavior |= NSWindowCollectionBehaviorFullScreenAuxiliary;
        }
        if ((combo.flags & kStationary) != 0U) {
            behavior |= NSWindowCollectionBehaviorStationary;
        }
        if ((combo.flags & kJoinAllApplications) != 0U) {
            behavior |= NSWindowCollectionBehaviorCanJoinAllApplications;
        }
        CuriaProbePanel* window = panelFrom(panel);
        window.collectionBehavior = behavior;
        window.level = combo.level;
    }
}

void showPanel(void* panel) {
    @autoreleasepool {
        [panelFrom(panel) orderFrontRegardless];
    }
}

void hidePanel(void* panel) {
    @autoreleasepool {
        [panelFrom(panel) orderOut:nil];
    }
}

void setIgnoresMouse(void* panel, bool ignore) {
    panelFrom(panel).ignoresMouseEvents = ignore;
}

bool panelVisible(void* panel) {
    return panelFrom(panel).isVisible;
}

std::string placeWindow(void* nswindow, int margin, int displayIndex) {
    @autoreleasepool {
        NSWindow* window = (__bridge NSWindow*)nswindow;
        NSArray<NSScreen*>* screens = NSScreen.screens;
        const NSPoint pointer = NSEvent.mouseLocation;
        NSScreen* screen = nil;
        if (displayIndex >= 0 && displayIndex < static_cast<int>(screens.count)) {
            screen = screens[static_cast<NSUInteger>(displayIndex)];
        } else {
            for (NSScreen* candidate in screens) {
                if (NSPointInRect(pointer, candidate.frame)) {
                    screen = candidate;
                    break;
                }
            }
        }
        if (screen == nil) {
            screen = screens.firstObject;
        }
        if (screen == nil) {
            return "placed=0";
        }
        const NSRect visible = screen.visibleFrame;
        const NSPoint origin = NSMakePoint(NSMaxX(visible) - window.frame.size.width - margin,
                                           NSMaxY(visible) - window.frame.size.height - margin);
        [window setFrameOrigin:origin];
        char frame[96];
        std::snprintf(frame, sizeof frame, "%.0f,%.0f,%.0f,%.0f", screen.frame.origin.x,
                      screen.frame.origin.y, screen.frame.size.width, screen.frame.size.height);
        char point[48];
        std::snprintf(point, sizeof point, "%.0f,%.0f", pointer.x, pointer.y);
        return fields({kv("placed", 1LL),
                       kv("screen_index", static_cast<long long>([screens indexOfObject:screen])),
                       kv("screen_frame", frame), kv("pointer", point),
                       kv("requested_display", static_cast<long long>(displayIndex))});
    }
}

std::string displaysSnapshot() {
    std::string out;
    int index = 0;
    for (const CGDirectDisplayID id : activeDisplays()) {
        char text[160];
        std::snprintf(text, sizeof text, "id=%u bounds=%s main=%d captured=%d",
                      static_cast<unsigned>(id), rectText(CGDisplayBounds(id)).c_str(),
                      CGDisplayIsMain(id) ? 1 : 0, displayCaptured(id) ? 1 : 0);
        if (!out.empty()) {
            out += ' ';
        }
        out += kv("display" + std::to_string(index++), text);
    }
    return out.empty() ? "displays=none" : out;
}

std::string focusSnapshot(void* panelOrNull) {
    @autoreleasepool {
        NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
        std::string captured;
        for (const CGDirectDisplayID id : activeDisplays()) {
            captured += displayCaptured(id) ? '1' : '0';
        }
        std::string out = fields({
            kv("front_name", str(front.localizedName)),
            kv("front_exec", str(front.executableURL.lastPathComponent)),
            kv("front_pid", static_cast<long long>(front.processIdentifier)),
            kv("front_bundle", str(front.bundleIdentifier)),
            kv("app_active", NSApplication.sharedApplication.isActive ? 1LL : 0LL),
            kv("policy", policyName(NSApplication.sharedApplication.activationPolicy)),
            kv("displays_captured", captured),
        });
        if (panelOrNull != nullptr) {
            CuriaProbePanel* panel = panelFrom(panelOrNull);
            char behavior[32];
            std::snprintf(behavior, sizeof behavior, "0x%llx",
                          static_cast<unsigned long long>(panel.collectionBehavior));
            NSNumber* screenNumber = panel.screen.deviceDescription[@"NSScreenNumber"];
            out += ' ';
            out += fields({
                kv("panel_visible", panel.isVisible ? 1LL : 0LL),
                kv("panel_key", panel.isKeyWindow ? 1LL : 0LL),
                kv("panel_on_active_space", panel.isOnActiveSpace ? 1LL : 0LL),
                kv("panel_occlusion_visible",
                   (panel.occlusionState & NSWindowOcclusionStateVisible) != 0 ? 1LL : 0LL),
                kv("panel_level", static_cast<long long>(panel.level)),
                kv("panel_behavior", behavior),
                kv("panel_ignores_mouse", panel.ignoresMouseEvents ? 1LL : 0LL),
                kv("panel_display", screenNumber != nil ? screenNumber.longLongValue : -1LL),
                kv("panel_frame", rectText(NSRectToCGRect(panel.frame))),
            });
        }
        return out;
    }
}

std::string permissionSnapshot() {
    // None of these three calls shows a prompt: they only report the current authorization.
    return fields({
        kv("screen_capture", CGPreflightScreenCaptureAccess() ? 1LL : 0LL),
        kv("listen_event", CGPreflightListenEventAccess() ? 1LL : 0LL),
        kv("accessibility", AXIsProcessTrusted() ? 1LL : 0LL),
    });
}

std::string environmentSnapshot() {
    @autoreleasepool {
#if defined(__arm64__)
        const char* arch = "arm64";
#else
        const char* arch = "x86_64";
#endif
        return fields({
            kv("macos", str(NSProcessInfo.processInfo.operatingSystemVersionString)),
            kv("arch", arch),
            kv("pid", static_cast<long long>(getpid())),
            kv("bundle", str(NSBundle.mainBundle.bundleIdentifier)),
            kv("screens", static_cast<long long>(NSScreen.screens.count)),
            kv("separate_spaces", [NSScreen screensHaveSeparateSpaces] ? 1LL : 0LL),
        });
    }
}

std::string ck3Snapshot() {
    @autoreleasepool {
        NSArray<NSRunningApplication*>* apps = ck3Applications();
        if (apps.count == 0) {
            return "ck3_running=0";
        }
        NSRunningApplication* app = apps.firstObject;
        return fields({
            kv("ck3_running", static_cast<long long>(apps.count)),
            kv("ck3_pid", static_cast<long long>(app.processIdentifier)),
            kv("ck3_bundle", str(app.bundleIdentifier)),
            kv("ck3_policy", policyName(app.activationPolicy)),
            kv("ck3_active", app.isActive ? 1LL : 0LL),
            kv("ck3_hidden", app.isHidden ? 1LL : 0LL),
        });
    }
}

std::string ck3WindowSnapshot() {
    @autoreleasepool {
        NSArray<NSRunningApplication*>* apps = ck3Applications();
        if (apps.count == 0) {
            return "ck3_windows=none";
        }
        const pid_t pid = apps.firstObject.processIdentifier;
        NSArray* list =
            CFBridgingRelease(CGWindowListCopyWindowInfo(kCGWindowListOptionAll, kCGNullWindowID));
        if (list == nil) {
            return "window_list=nil";
        }
        const std::vector<CGDirectDisplayID> displays = activeDisplays();
        std::string out = kv("window_list_total", static_cast<long long>(list.count));
        int shown = 0;
        long long matches = 0;
        for (NSDictionary* window in list) {
            if ([window[(id)kCGWindowOwnerPID] intValue] != pid) {
                continue;
            }
            ++matches;
            if (shown >= 6) {
                continue;
            }
            CGRect rect = CGRectZero;
            NSDictionary* bounds = window[(id)kCGWindowBounds];
            const bool haveRect = bounds != nil && CGRectMakeWithDictionaryRepresentation(
                                                       (__bridge CFDictionaryRef)bounds, &rect);
            long long displayId = -1;
            if (haveRect) {
                const CGPoint center = CGPointMake(CGRectGetMidX(rect), CGRectGetMidY(rect));
                for (const CGDirectDisplayID id : displays) {
                    if (CGRectContainsPoint(CGDisplayBounds(id), center)) {
                        displayId = static_cast<long long>(id);
                        break;
                    }
                }
            }
            char text[200];
            std::snprintf(
                text, sizeof text, "%s layer=%d onscreen=%d name=%d owner_name=%d display=%lld",
                haveRect ? rectText(rect).c_str() : "none", [window[(id)kCGWindowLayer] intValue],
                [window[(id)kCGWindowIsOnscreen] intValue],
                [window[(id)kCGWindowName] length] > 0 ? 1 : 0,
                [window[(id)kCGWindowOwnerName] length] > 0 ? 1 : 0, displayId);
            out += ' ';
            out += kv("win" + std::to_string(shown), text);
            ++shown;
        }
        out += ' ';
        out += kv("ck3_windows", matches);
        return out;
    }
}

bool frontmostIsCk3() {
    @autoreleasepool {
        NSString* name =
            NSWorkspace.sharedWorkspace.frontmostApplication.executableURL.lastPathComponent;
        return name != nil && [name caseInsensitiveCompare:@"ck3"] == NSOrderedSame;
    }
}

void rememberFrontmost() {
    @autoreleasepool {
        NSRunningApplication* front = NSWorkspace.sharedWorkspace.frontmostApplication;
        if (front != nil && front.processIdentifier != getpid()) {
            remembered() = front;
        }
    }
}

bool reactivateRemembered() {
    @autoreleasepool {
        NSRunningApplication* app = remembered();
        if (app == nil) {
            return false;
        }
        return [app activateWithOptions:NSApplicationActivateAllWindows];
    }
}

std::string rememberedLabel() {
    @autoreleasepool {
        NSRunningApplication* app = remembered();
        return app != nil ? appLabel(app) : std::string("remembered=none");
    }
}

std::vector<int> otherProbePids() {
    @autoreleasepool {
        std::vector<int> pids;
        NSString* bundle = NSBundle.mainBundle.bundleIdentifier;
        if (bundle == nil) {
            return pids;
        }
        for (NSRunningApplication* app in
             [NSRunningApplication runningApplicationsWithBundleIdentifier:bundle]) {
            if (app.processIdentifier != getpid()) {
                pids.push_back(static_cast<int>(app.processIdentifier));
            }
        }
        return pids;
    }
}

void installWorkspaceObservers(std::function<void(const std::string&)> onEvent) {
    auto callback = std::make_shared<std::function<void(const std::string&)>>(std::move(onEvent));
    NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;

    const auto observeApp = [&](NSNotificationName name, const char* label) {
        id token = [center addObserverForName:name
                                       object:nil
                                        queue:NSOperationQueue.mainQueue
                                   usingBlock:^(NSNotification* note) {
                                     NSRunningApplication* app =
                                         note.userInfo[NSWorkspaceApplicationKey];
                                     (*callback)(fields({kv("event", label), appLabel(app)}));
                                   }];
        [observerTokens() addObject:token];
    };
    observeApp(NSWorkspaceDidActivateApplicationNotification, "app_activated");
    observeApp(NSWorkspaceDidDeactivateApplicationNotification, "app_deactivated");

    id spaceToken = [center addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification
                                        object:nil
                                         queue:NSOperationQueue.mainQueue
                                    usingBlock:^(NSNotification* /*note*/) {
                                      (*callback)(kv("event", "active_space_changed"));
                                    }];
    [observerTokens() addObject:spaceToken];
}

void removeWorkspaceObservers() {
    NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
    for (id token in observerTokens()) {
        [center removeObserver:token];
    }
    [observerTokens() removeAllObjects];
}

bool registerHotkeys(std::function<void(int)> onHotkey, std::string* error) {
    hotkeyCallback() = std::move(onHotkey);

    const EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
    const OSStatus installed = InstallEventHandler(GetApplicationEventTarget(), &hotkeyHandler, 1,
                                                   &spec, nullptr, nullptr);
    if (installed != noErr) {
        if (error != nullptr) {
            *error = "InstallEventHandler failed: " + std::to_string(installed);
        }
        return false;
    }

    struct Key {
        UInt32 id;
        UInt32 code;
        const char* name;
    };
    const Key keys[] = {{1, kVK_ANSI_K, "Ctrl+Cmd+K"},
                        {2, kVK_ANSI_L, "Ctrl+Cmd+L"},
                        {3, kVK_ANSI_J, "Ctrl+Cmd+J"},
                        {4, kVK_ANSI_H, "Ctrl+Cmd+H"}};
    bool ok = true;
    for (const Key& key : keys) {
        EventHotKeyRef ref = nullptr;
        const EventHotKeyID id = {'CRPB', key.id};
        const OSStatus status = RegisterEventHotKey(key.code, cmdKey | controlKey, id,
                                                    GetApplicationEventTarget(), 0, &ref);
        if (status != noErr) {
            ok = false;
            if (error != nullptr) {
                *error += std::string(key.name) + " failed: " + std::to_string(status) + "; ";
            }
        }
    }
    return ok;
}

void playSound(const std::string& name) {
    @autoreleasepool {
        static NSMutableDictionary<NSString*, NSSound*>* cache = [NSMutableDictionary dictionary];
        NSString* key = [NSString stringWithUTF8String:name.c_str()];
        NSSound* sound = cache[key];
        if (sound == nil) {
            sound = [NSSound soundNamed:key];
            if (sound != nil) {
                cache[key] = sound;
            }
        }
        if (sound != nil) {
            [sound stop];
            [sound play];
        } else {
            NSBeep();
        }
    }
}

std::string homeDirectory() {
    @autoreleasepool {
        return str(NSHomeDirectory());
    }
}

}  // namespace curia::probe::mac
