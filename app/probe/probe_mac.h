// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "probe_combos.h"

// The AppKit, Carbon and CoreGraphics calls of the M2 overlay probe. No Objective-C in this header;
// windows cross the boundary as opaque pointers (an NSWindow*, or for panels the pointer returned
// by createPanel, which is retained until releasePanel).
namespace curia::probe::mac {

// Activation policy of the running NSApplication (call after SDL_Init).
bool setAccessoryPolicy();
std::string activationPolicyName();

// Our own borderless, non-activating NSPanel (not visible yet). `transparent` clears the
// background and drops the shadow so SDL treats the wrapped window as transparent.
void* createPanel(int width, int height, bool transparent);
void releasePanel(void* panel);

// Level and collection behaviour of a combination; does not change visibility.
void applyCombo(void* panel, const Combo& combo);
void showPanel(void* panel);  // orderFrontRegardless: fronts the panel without activating the app
void hidePanel(void* panel);  // orderOut
void setIgnoresMouse(void* panel, bool ignore);
bool panelVisible(void* panel);

// Moves an NSWindow (our panel, or SDL's plain window) to the top-right corner of a display:
// `displayIndex` indexes NSScreen.screens, -1 means the display under the mouse pointer. Returns a
// key=value description of the choice.
std::string placeWindow(void* nswindow, int margin, int displayIndex);

// One line of key=value text per call.
std::string focusSnapshot(void* panelOrNull);  // frontmost app, whether we are active, panel state
std::string permissionSnapshot();              // screen-capture and input-monitoring pre-checks
std::string environmentSnapshot();             // OS, screens, "separate Spaces" setting, bundle id
std::string displaysSnapshot();                // active displays: id, bounds, main, captured
std::string ck3Snapshot();                     // running process whose executable is "ck3"
std::string ck3WindowSnapshot();               // CGWindowList bounds for that process (detect only)
bool frontmostIsCk3();

// Remembers the frontmost application when it is not this one; reactivateRemembered() asks the
// system to make it frontmost again (the E7 escalation) and returns whether the request was
// accepted.
void rememberFrontmost();
bool reactivateRemembered();
std::string rememberedLabel();

// Process ids of other running instances of this bundle (empty when unbundled).
std::vector<int> otherProbePids();

// NSWorkspace activation and Space-change notifications, delivered on the main thread.
void installWorkspaceObservers(std::function<void(const std::string&)> onEvent);
void removeWorkspaceObservers();

// Installs the observers, lets the installing function return, then posts one synthetic activation,
// deactivation and Space-change notification and checks that the callback ran for each. Touches no
// window and no focus. `report` gets a one-line result.
bool selfTestWorkspaceObservers(std::string* report);

// Carbon global hot keys Ctrl+Cmd+K / L / J / H, ids 1..4. Returns false and fills `error` when a
// registration fails; `onHotkey` runs on the main thread.
bool registerHotkeys(std::function<void(int)> onHotkey, std::string* error);

// A macOS system sound by name ("Tink", "Glass", ...) through AudioServices; falls back to the
// system beep.
void playSound(const std::string& name);
std::string homeDirectory();

}  // namespace curia::probe::mac
