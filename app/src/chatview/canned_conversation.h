// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bridge/frame.h"
#include "providers/provider.h"

// The conversation `curia --smoke` draws: two synthetic readings of a realm (invented names) and
// two questions, answered by a provider that plays a script. No network, no key, no game. The first
// answer uses every kind of markup the chat view handles (bold, a heading, a numbered list of three
// actions, a bullet, inline code, a word too long for a line, names with diacritics and in
// Cyrillic); the second one fails part way, so the error and its Retry button are drawn too.
namespace curia::chatview::canned {

inline constexpr std::string_view kFirstQuestion = "What should I do first?";
inline constexpr std::string_view kSecondQuestion = "Who might rebel against me?";

// 3 October 1071, the first click on Advisor.
bridge::Frame firstFrame();
// 14 November 1071, a later click: a divider row in the conversation.
bridge::Frame secondFrame();

std::string firstAnswer();
// The part of the second answer that arrives before the error.
std::string secondAnswerStart();
// The error that ends the second answer (the service is busy; a retry is offered).
providers::ChatError secondAnswerError();

// `text` cut into pieces of at most `maxBytes` bytes (at least one character each), never inside a
// UTF-8 sequence: how a provider delivers an answer.
std::vector<std::string> splitIntoPieces(std::string_view text, std::size_t maxBytes);

// A provider that plays scripted answers, one piece per step(), on the thread that calls step()
// (the Provider contract allows that: the handlers only post to the mailbox). Each streamChat()
// takes the next answer of the script; a request beyond the script fails with Code::Other. A
// cancelled request ends with Code::Cancelled and the text sent so far.
class ScriptedProvider final : public providers::Provider {
public:
    struct Answer {
        std::vector<std::string> pieces;
        providers::Usage usage;
        // Ends the answer instead of onDone.
        std::optional<providers::ChatError> error;
    };

    explicit ScriptedProvider(std::vector<Answer> answers);

    void streamChat(const providers::ChatRequest& request, providers::StreamHandlers handlers,
                    providers::CancelToken cancel) override;

    // Delivers the next piece of the request in progress, or its end. False when none is in
    // progress.
    bool step();
    [[nodiscard]] bool busy() const { return active_.has_value(); }
    [[nodiscard]] std::size_t requests() const { return requests_; }

private:
    struct Active {
        providers::StreamHandlers handlers;
        providers::CancelToken cancel;
        std::optional<std::size_t> answer;
        std::size_t nextPiece = 0;
        std::string sent;
    };

    void finish(const std::optional<providers::ChatError>& error, const providers::Usage& usage);

    std::vector<Answer> answers_;
    std::size_t nextAnswer_ = 0;
    std::size_t requests_ = 0;
    std::optional<Active> active_;
};

// The script of the smoke test: the first answer in pieces of 9 bytes, the second one's start and
// its error.
std::vector<ScriptedProvider::Answer> smokeScript();

}  // namespace curia::chatview::canned
