// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <ostream>
#include <string>
#include <string_view>

#include "advisor/plain_words.h"
#include "chatview/canned_conversation.h"
#include "chatview/view_model.h"
#include "chatview/view_text.h"
#include "core/app_controller.h"
#include "core/dev_config.h"
#include "core/secret.h"

using curia::advisor::findInternalWord;
namespace text = curia::chatview::text;

// The plain-words rule (FR-OVL-11, advisor/plain_words.h) over every text the chat window shows of
// its own (the advisor's texts are checked in test_advisor_plain_words.cpp).

namespace {

void checkPlain(std::string_view what, std::string_view value) {
    CAPTURE(what);
    CAPTURE(value);
    const std::string_view found = findInternalWord(value);
    CHECK_MESSAGE(found.empty(), "internal word: " << std::string(found));
}

}  // namespace

TEST_CASE("plain words: every fixed text of the chat window") {
    for (const std::string_view value : text::kAllFixedTexts) {
        CHECK_FALSE(value.empty());
        checkPlain("view text", value);
    }
    checkPlain("seen from", text::seenFrom({1071, 10, 3}));
    checkPlain("seen from, an odd date", text::seenFrom({867, 0, 0}));
    CHECK((text::seenFrom({1066, 9, 15}) == "What I can see is from 15 September 1066."));
}

TEST_CASE("plain words: every text a built view can hold") {
    // Every banner and status a view can show, built through buildChatView.
    for (const bool folder : {true, false}) {
        for (const bool snapshot : {true, false}) {
            for (const bool documents : {true, false}) {
                for (const auto notice :
                     {curia::advisor::Notice::None, curia::advisor::Notice::RestartCk3,
                      curia::advisor::Notice::SilentSoft, curia::advisor::Notice::Ck3NotRunning}) {
                    curia::chatview::ViewInputs inputs;
                    inputs.gameFolderFound = folder;
                    inputs.ck3FolderInDocuments = documents;
                    inputs.hasSnapshot = snapshot;
                    inputs.date = curia::bridge::Date{1071, 10, 3};
                    inputs.unreadable = true;
                    inputs.probe = true;
                    inputs.notice = notice;
                    for (const bool unsupported : {true, false}) {
                        inputs.unsupportedFormat = unsupported;
                        const curia::chatview::ChatView view =
                            curia::chatview::buildChatView(inputs);
                        checkPlain("status", view.status);
                        checkPlain("empty conversation", view.emptyState);
                        for (const auto& banner : view.banners) {
                            checkPlain("banner", banner.text);
                        }
                        checkPlain("pause reminder",
                                   curia::chatview::plainText(view.pauseReminder));
                        checkPlain("input hint", view.inputHint);
                    }
                }
            }
        }
    }
}

TEST_CASE("plain words: the not-configured texts the window can show") {
    checkPlain("default", curia::kDefaultNotConfigured);
    // What a development build says for each missing or bad variable (names only).
    curia::SecretRegistry registry;
    const auto config = [&](const char* key, const char* model) {
        const curia::platform::GetEnv env = [=](const char* name) -> std::string {
            const std::string wanted(name);
            if (wanted == "CURIA_ANTHROPIC_API_KEY") {
                return key;
            }
            if (wanted == "CURIA_MODEL") {
                return model;
            }
            return {};
        };
        return curia::describeNotConfigured(curia::loadDevConfig(env, registry));
    };
    checkPlain("nothing set", config("", ""));
    checkPlain("no model", config("sk-ant-FAKE-plain-words-0000", ""));
    checkPlain("bad key", config("short", "mock-model-1"));
    checkPlain("bad model", config("sk-ant-FAKE-plain-words-0001", "bad model name!"));
    // A key the registry refuses (a part of its own "[redacted]" marker) cannot be kept out of
    // what Curia writes.
    const std::string refused = config("redacted", "mock-model-1");
    CHECK((refused.find("not usable") != std::string::npos));
    checkPlain("key the registry refuses", refused);
    // A build without the development configuration.
    curia::DevConfig disabled;
    disabled.problems.emplace_back(curia::kDevConfigDisabledProblem);
    checkPlain("disabled build", curia::describeNotConfigured(disabled));
}

TEST_CASE("plain words: the canned smoke conversation") {
    namespace canned = curia::chatview::canned;
    checkPlain("first question", canned::kFirstQuestion);
    checkPlain("second question", canned::kSecondQuestion);
    checkPlain("first answer", canned::firstAnswer());
    checkPlain("second answer", canned::secondAnswerStart());
}
