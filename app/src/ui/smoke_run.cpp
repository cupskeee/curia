// SPDX-License-Identifier: MIT
#include "ui/smoke_run.h"

#include <imgui.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "advisor/transcript.h"
#include "chatview/canned_conversation.h"
#include "chatview/utf8.h"
#include "chatview/view_model.h"
#include "chatview/view_text.h"
#include "core/app_controller.h"
#include "core/dev_config.h"
#include "ui/chat_window.h"
#include "ui/sdl_wake.h"

namespace curia::ui {

namespace {

namespace canned = chatview::canned;

// More than any scripted answer needs; a provider that never ends must not hang the test.
constexpr int kMaxFramesPerAnswer = 400;
// Frames after the last answer, for the scroll position to settle.
constexpr int kSettleFrames = 3;

int fail(const std::string& reason) {
    std::fprintf(stderr, "CURIA_SMOKE_FAIL %s\n", reason.c_str());
    return 1;
}

// Every text the smoke run shows: the conversation, the window's own texts and the last view.
std::vector<std::string> shownTexts(const chatview::ChatView& view) {
    std::vector<std::string> texts = {std::string(canned::kFirstQuestion),
                                      std::string(canned::kSecondQuestion),
                                      canned::firstAnswer(),
                                      canned::secondAnswerStart(),
                                      view.status,
                                      view.emptyState,
                                      chatview::plainText(view.pauseReminder),
                                      view.inputHint};
    for (const std::string_view fixed : chatview::text::kAllFixedTexts) {
        texts.emplace_back(fixed);
    }
    for (const chatview::Banner& banner : view.banners) {
        texts.push_back(banner.text);
    }
    for (const chatview::ViewItem& item : view.items) {
        texts.push_back(item.text);
        texts.push_back(item.note);
        for (const chatview::AnswerLine& line : item.answer.lines) {
            texts.push_back(line.marker);
            for (const chatview::Run& run : line.runs) {
                texts.push_back(run.text);
            }
        }
    }
    return texts;
}

// The characters that one of the two faces lacks (line breaks and tabs are not drawn).
std::set<char32_t> missingGlyphs(const FontSet& fonts, const std::vector<std::string>& texts) {
    std::set<char32_t> missing;
    for (const std::string& value : texts) {
        for (const char32_t c : chatview::decodeUtf8(value)) {
            if (c == '\n' || c == '\r' || c == '\t') {
                continue;
            }
            // ImWchar is 16 bits wide in this build: a character beyond the BMP cannot be drawn.
            const bool inRange = c <= 0xFFFF;
            const auto glyph = static_cast<ImWchar>(inRange ? c : 0);
            if (!inRange || !fonts.regular->IsGlyphInFont(glyph) ||
                !fonts.bold->IsGlyphInFont(glyph)) {
                missing.insert(c);
            }
        }
    }
    return missing;
}

}  // namespace

int runSmoke(const SmokeContext& context) {
    IRenderBackend& backend = *context.backend;
    if (!context.fonts.bundled) {
        return fail("font=default (the bundled font did not load)");
    }

    SdlWake wake(context.wakeEvent);
    AppController::Options options;
    options.wake = wake.function();
    AppController controller(options);
    const auto provider = std::make_shared<canned::ScriptedProvider>(canned::smokeScript());
    if (!controller.setProvider(provider, "scripted-smoke-model")) {
        return fail("the scripted provider was refused");
    }

    int result = 0;
    std::uint64_t frames = 0;
    {
        ChatWindow chat(controller, context.fonts, ChatWindow::Options{});
        // One frame as the window loop draws it: events first (the wake-up event leads to a pump,
        // as in the app), then the view.
        auto frame = [&] {
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == wake.eventType()) {
                    controller.pump();
                } else {
                    backend.processEvent(event);
                }
            }
            controller.pump();
            backend.beginFrame();
            chat.draw();
            backend.endFrame();
            ++frames;
        };
        auto ask = [&](std::string_view question) {
            chat.setDraft(question);
            frame();
            if (!chat.submit()) {
                return false;
            }
            for (int i = 0; i < kMaxFramesPerAnswer && provider->busy(); ++i) {
                provider->step();
                frame();
            }
            frame();
            return !provider->busy();
        };

        frame();
        frame();
        if (context.scene != SmokeScene::Start) {
            // As if the player had clicked Advisor in the game.
            controller.onMessage(
                UiMessage{makeWatcherEvent(bridge::FrameComplete{canned::firstFrame()})});
            controller.onOverlayOpened(OverlaySource::GameClick);
            if (!ask(canned::kFirstQuestion)) {
                result = fail("the first question did not get its answer");
            }
            // A later click: a divider row, then a question whose answer fails.
            controller.onMessage(
                UiMessage{makeWatcherEvent(bridge::FrameComplete{canned::secondFrame()})});
            if (result == 0 && !ask(canned::kSecondQuestion)) {
                result = fail("the second question did not end");
            }
        }
        if (result == 0 && context.scene == SmokeScene::Notices) {
            // A reading from the test button (an information and one more divider), then a
            // damaged reading, then the key missing (the service is no longer set up): three
            // notices over the conversation.
            bridge::Frame probe = canned::secondFrame();
            probe.mode = 1;
            ++probe.snap;
            controller.onMessage(UiMessage{makeWatcherEvent(bridge::FrameComplete{probe})});
            controller.onMessage(UiMessage{makeWatcherEvent(
                bridge::FrameDamaged{probe.snap + 1, bridge::DamageReason::Incomplete, "smoke"})});
            DevConfig missingKey;
            missingKey.model = "scripted-smoke-model";
            controller.setNotConfigured(describeNotConfigured(missingKey));
        }
        for (int i = 0; i < kSettleFrames; ++i) {
            frame();
        }

        const std::vector<advisor::TranscriptEntry>& transcript = controller.session().transcript();
        const chatview::ChatView& view = chat.lastView();
        if (result == 0 && context.scene == SmokeScene::Start &&
            (!view.items.empty() || view.emptyState.empty() || !view.banners.empty())) {
            result = fail("the start scene did not show the empty window");
        }
        if (result == 0 && context.scene != SmokeScene::Start &&
            (transcript.size() < 5 || transcript[1].end != advisor::AnswerEnd::Complete ||
             transcript[2].kind != advisor::EntryKind::Divider ||
             transcript[4].end != advisor::AnswerEnd::Failed || view.items.size() < 5 ||
             !view.items[1].offerCopy)) {
            result = fail("the conversation did not end as scripted");
        }
        if (result == 0 && context.scene == SmokeScene::Conversation &&
            (transcript.size() != 5 || view.items.size() != 5 || !view.items[4].offerRetry)) {
            result = fail("the conversation did not end as scripted");
        }
        if (result == 0 && context.scene == SmokeScene::Notices && view.banners.size() != 3) {
            result = fail("the notices scene did not show its three notices");
        }

        const std::set<char32_t> missing = missingGlyphs(context.fonts, shownTexts(view));
        if (result == 0 && !missing.empty()) {
            std::string list;
            for (const char32_t c : missing) {
                char code[16];
                std::snprintf(code, sizeof(code), " U+%04X", static_cast<unsigned>(c));
                list += code;
            }
            result = fail("glyphs_missing=" + std::to_string(missing.size()) + list);
        }

        if (result == 0 && !context.screenshotPath.empty()) {
            backend.captureNextFrame(context.screenshotPath);
            frame();
            if (!backend.captureSaved()) {
                result = fail("the screenshot could not be saved");
            }
        }
        controller.onStop();
    }
    wake.disarm();

    if (result == 0) {
        std::printf("CURIA_SMOKE_OK frames=%llu renderer=%s ui_scale=%.2f font=bundled "
                    "glyphs_missing=0\n",
                    static_cast<unsigned long long>(frames), backend.rendererName().c_str(),
                    static_cast<double>(context.uiScale));
    }
    return result;
}

}  // namespace curia::ui
