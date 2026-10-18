// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>

#include <memory>
#include <string_view>

// The global hot key that shows and hides the overlay (FR-OVL-7): Ctrl+Cmd+K on macOS through the
// Carbon call RegisterEventHotKey, which fired with CK3 frontmost and asked for no permission in M2
// (it needs neither Accessibility nor Input Monitoring, unlike an event tap or a global event
// monitor, which Curia never uses). Windows and Linux get no hot key in M4: the in-game Advisor
// button is the trigger there.
//
// The Carbon handler does one thing: it pushes one SDL event of the type given to install(), so
// that SDL_WaitEvent wakes at once and the run loop toggles the overlay. Carbon calls it on the
// main thread while SDL pumps the system's events. Its storage is static and lives for the
// process: nothing a handler call could reach goes away while it is installed. UI thread only.
namespace curia::ui {

class HotKey {
public:
    // Installs the handler and, if `grab`, registers Ctrl+Cmd+K for this process, exclusively
    // (kEventHotKeyExclusive: while Curia holds it, other registrants of the combination are not
    // told of a press). Null where there is no hot key (not macOS) or when the handler cannot be
    // installed. `status` gets one short word for the app log: "registered", "taken" (the
    // combination is already registered, by a second Curia or another app: not fatal, the overlay
    // still opens from the game and the tray), "failed", "not_grabbed" (grab false) or
    // "unsupported".
    static std::unique_ptr<HotKey> install(Uint32 eventType, bool grab, std::string_view& status);

    // Unregisters the key and removes the handler; nothing is pushed after this.
    ~HotKey();
    HotKey(const HotKey&) = delete;
    HotKey& operator=(const HotKey&) = delete;

    // The key itself was registered (not only the handler installed).
    bool registered() const { return registered_; }

    // For the self-test: hands the installed handler a hot-key event of our own, as the system
    // would on a press. False if the event could not be made or sent.
    static bool sendTestPress();

private:
    HotKey() = default;
    bool registered_ = false;
};

// `curia --selftest-hotkey`: no window, the key is not grabbed (the player's Ctrl+Cmd+K stays where
// it is). Installs the handler from a function that returns, sends a synthetic press, checks that
// exactly one event of the registered type reached the SDL queue, removes the handler, sends again
// and checks that nothing arrives. Prints CURIA_SELFTEST_OK or CURIA_SELFTEST_FAIL; exit 0 or 1.
int runHotKeySelfTest();

}  // namespace curia::ui
