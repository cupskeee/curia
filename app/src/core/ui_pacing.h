// SPDX-License-Identifier: MIT
#pragma once

namespace curia {

// How the UI thread waits for its next event. The app is event-driven: it blocks in
// SDL_WaitEvent and uses almost no CPU while nothing happens (NFR-5). It wakes on a timer only
// while something on screen changes by itself.
struct WaitPolicy {
    enum class Kind {
        // SDL_WaitEvent: sleep until an event arrives.
        Block,
        // SDL_WaitEventTimeout(timeoutMs). 0 = do not wait at all.
        Timeout,
    };
    Kind kind = Kind::Block;
    int timeoutMs = 0;

    friend bool operator==(const WaitPolicy& a, const WaitPolicy& b) {
        return a.kind == b.kind && a.timeoutMs == b.timeoutMs;
    }
};

// The pause between redraws of the busy indicator (about 30 frames per second).
inline constexpr int kSpinnerFrameMs = 33;
// The pause between redraws that keeps a text cursor blinking.
inline constexpr int kCaretBlinkMs = 250;

// The rule:
//   * messages are pending: Timeout(0), handle them now (also when the overlay is hidden);
//   * the overlay is hidden: Block (a hidden overlay draws nothing, whatever is going on; the
//     mailbox's wake-up event still ends the wait);
//   * a spinner is active: Timeout(kSpinnerFrameMs);
//   * a text box has focus (its cursor blinks): Timeout(kCaretBlinkMs);
//   * otherwise: Block.
// With both a spinner and a text box the shorter wait wins.
WaitPolicy uiWaitPolicy(bool wantsTextInput, bool spinnerActive, bool overlayHidden,
                        bool pendingMessages);

}  // namespace curia
