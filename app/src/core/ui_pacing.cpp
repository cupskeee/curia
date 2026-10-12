// SPDX-License-Identifier: MIT
#include "core/ui_pacing.h"

namespace curia {

WaitPolicy uiWaitPolicy(bool wantsTextInput, bool spinnerActive, bool overlayHidden,
                        bool pendingMessages) {
    if (pendingMessages) {
        return {WaitPolicy::Kind::Timeout, 0};
    }
    if (overlayHidden) {
        return {WaitPolicy::Kind::Block, 0};
    }
    if (spinnerActive) {
        return {WaitPolicy::Kind::Timeout, kSpinnerFrameMs};
    }
    if (wantsTextInput) {
        return {WaitPolicy::Kind::Timeout, kCaretBlinkMs};
    }
    return {WaitPolicy::Kind::Block, 0};
}

}  // namespace curia
