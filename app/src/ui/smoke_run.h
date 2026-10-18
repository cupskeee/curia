// SPDX-License-Identifier: MIT
#pragma once

#include <SDL3/SDL.h>

#include <string>

#include "core/cli.h"
#include "ui/render_backend.h"
#include "ui/theme.h"

namespace curia::ui {

struct SmokeContext {
    SDL_Window* window = nullptr;
    // Not owned.
    IRenderBackend* backend = nullptr;
    FontSet fonts;
    float uiScale = 1.0F;
    // The event type the mailbox's wake-up pushes.
    Uint32 wakeEvent = 0;
    // Save the last frame as a BMP file here; empty = no screenshot.
    std::string screenshotPath;
    // What to draw (--smoke-scene).
    SmokeScene scene = SmokeScene::Conversation;
};

// `curia --smoke`: the real controller and chat window with the canned conversation
// (chatview/canned_conversation.h): a synthetic reading as if the player had clicked Advisor, a
// question with a streamed answer, a second reading (a divider row) and a question whose answer
// fails (Retry). The notices scene then adds a reading from the test button, a damaged reading and
// a missing variable, so the window shows its notices; the start scene draws the window before
// anything has come from the game. No environment variable is read, no CK3 folder is looked for,
// nothing goes to the network and no key exists. Prints one line,
//   CURIA_SMOKE_OK frames=N renderer=NAME ui_scale=S font=bundled glyphs_missing=0
// (the first three fields as before), and returns 0; returns 1 with a CURIA_SMOKE_FAIL line when
// the bundled font did not load, a character of the conversation or of the window's texts is
// missing from it, the scene did not end as scripted, or the screenshot failed.
int runSmoke(const SmokeContext& context);

}  // namespace curia::ui
