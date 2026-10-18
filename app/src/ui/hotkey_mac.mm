// SPDX-License-Identifier: MIT
// The Carbon hot key (ui/hotkey.h). Carbon's event manager only: no event tap, no global event
// monitor, no Accessibility call (docs/research-notes.md 2.5).
#import <Carbon/Carbon.h>
#include <SDL3/SDL.h>

#include <atomic>
#include <cstdio>
#include <memory>

#include "ui/hotkey.h"

namespace curia::ui {

namespace {

// The hot key's id: Curia's signature and number 1 (the one key).
constexpr OSType kSignature = (static_cast<OSType>('C') << 24U) |
                              (static_cast<OSType>('U') << 16U) | (static_cast<OSType>('R') << 8U) |
                              static_cast<OSType>('A');
constexpr UInt32 kToggleId = 1;

// What the handler reads, for the whole life of the process (static storage: nothing a call of the
// handler can reach is ever destroyed). 0 = nobody listens, push nothing.
std::atomic<Uint32> gEventType{0};
EventHandlerRef gHandler = nullptr;
EventHotKeyRef gKey = nullptr;

// Runs on the main thread, inside SDL's event pump. It only pushes an SDL event.
OSStatus onHotKey(EventHandlerCallRef /*next*/, EventRef event, void* /*userData*/) {
    EventHotKeyID id{};
    const OSStatus status = GetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID,
                                              nullptr, sizeof id, nullptr, &id);
    if (status != noErr || id.signature != kSignature || id.id != kToggleId) {
        return eventNotHandledErr;  // not ours
    }
    const Uint32 type = gEventType.load();
    if (type != 0) {
        SDL_Event pushed;
        SDL_zero(pushed);
        pushed.type = type;
        SDL_PushEvent(&pushed);
    }
    return noErr;
}

}  // namespace

std::unique_ptr<HotKey> HotKey::install(Uint32 eventType, bool grab, std::string_view& status) {
    if (gHandler != nullptr) {
        status = "failed";  // one at a time
        return nullptr;
    }
    const EventTypeSpec spec = {kEventClassKeyboard, kEventHotKeyPressed};
    if (InstallEventHandler(GetApplicationEventTarget(), &onHotKey, 1, &spec, nullptr, &gHandler) !=
        noErr) {
        gHandler = nullptr;
        status = "failed";
        return nullptr;
    }
    gEventType.store(eventType);
    std::unique_ptr<HotKey> hotKey(new HotKey());
    if (!grab) {
        status = "not_grabbed";
        return hotKey;
    }
    // Ctrl+Cmd+K: a combination with Cmd or Ctrl (Option-only ones were refused by macOS 15.0 and
    // 15.1, research-notes 2.5). Exclusively: without kEventHotKeyExclusive a second registrant
    // (a second Curia, say) would succeed too and every press would toggle both (CarbonEvents.h).
    const EventHotKeyID id = {kSignature, kToggleId};
    const OSStatus registered =
        RegisterEventHotKey(kVK_ANSI_K, cmdKey | controlKey, id, GetApplicationEventTarget(),
                            kEventHotKeyExclusive, &gKey);
    if (registered == noErr) {
        hotKey->registered_ = true;
        status = "registered";
    } else {
        gKey = nullptr;
        status = registered == eventHotKeyExistsErr ? "taken" : "failed";
    }
    return hotKey;
}

HotKey::~HotKey() {
    gEventType.store(0);
    if (gKey != nullptr) {
        UnregisterEventHotKey(gKey);
        gKey = nullptr;
    }
    if (gHandler != nullptr) {
        RemoveEventHandler(gHandler);
        gHandler = nullptr;
    }
}

bool HotKey::sendTestPress() {
    EventRef event = nullptr;
    if (CreateEvent(nullptr, kEventClassKeyboard, kEventHotKeyPressed, GetCurrentEventTime(),
                    kEventAttributeNone, &event) != noErr) {
        return false;
    }
    const EventHotKeyID id = {kSignature, kToggleId};
    bool sent = SetEventParameter(event, kEventParamDirectObject, typeEventHotKeyID, sizeof id,
                                  &id) == noErr;
    // With the handler removed nothing handles the event: that is the point of the second send.
    if (sent) {
        const OSStatus status = SendEventToEventTarget(event, GetApplicationEventTarget());
        sent = status == noErr || status == eventNotHandledErr;
    }
    ReleaseEvent(event);
    return sent;
}

namespace {

// Events of `type` waiting in SDL's queue (taken out of it).
int drain(Uint32 type) {
    SDL_PumpEvents();
    int count = 0;
    SDL_Event event;
    while (SDL_PeepEvents(&event, 1, SDL_GETEVENT, type, type) == 1) {
        ++count;
    }
    return count;
}

// Installs in a function of its own, so that the handler is called after it returned (the M2
// probe once crashed when an observer outlived what it pointed at).
std::unique_ptr<HotKey> installForTest(Uint32 type) {
    std::string_view status;
    return HotKey::install(type, false, status);
}

}  // namespace

int runHotKeySelfTest() {
    if (!SDL_Init(SDL_INIT_EVENTS)) {
        std::printf("CURIA_SELFTEST_FAIL hotkey sdl_init\n");
        return 1;
    }
    const Uint32 type = SDL_RegisterEvents(1);
    int result = 1;
    if (type == 0) {
        std::printf("CURIA_SELFTEST_FAIL hotkey register_events\n");
    } else {
        std::unique_ptr<HotKey> hotKey = installForTest(type);
        const bool sent = hotKey != nullptr && HotKey::sendTestPress();
        const int delivered = drain(type);
        hotKey.reset();
        const bool sentAfter = HotKey::sendTestPress();
        const int afterRemove = drain(type);
        const bool ok = sent && sentAfter && delivered == 1 && afterRemove == 0;
        std::printf("%s hotkey installed=%d delivered=%d after_remove=%d\n",
                    ok ? "CURIA_SELFTEST_OK" : "CURIA_SELFTEST_FAIL", sent ? 1 : 0, delivered,
                    afterRemove);
        result = ok ? 0 : 1;
    }
    SDL_Quit();
    return result;
}

}  // namespace curia::ui
