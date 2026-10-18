// SPDX-License-Identifier: MIT
#include "chatview/canned_conversation.h"

#include <utility>

#include "chatview/utf8.h"

namespace curia::chatview::canned {

namespace {

bridge::Frame baseFrame(std::int32_t snap, bridge::Date date, std::int32_t gold) {
    bridge::Frame frame;
    frame.snap = snap;
    frame.mode = 0;
    frame.modVersion = "0.2.1";
    frame.date = date;
    frame.rulerId = 1001;
    frame.rulerName = "\xC3\x86lfric Testsson";  // Ælfric
    frame.fin = bridge::Finances{gold, 78};
    bridge::Heir heir;
    heir.id = 1002;
    heir.opinion = 84;
    heir.name = "\xC3\x86lfwynn Testd\xC3\xB3ttir";  // Ælfwynn Testdóttir
    frame.heir = heir;
    frame.recordCount = 4;
    return frame;
}

}  // namespace

bridge::Frame firstFrame() {
    return baseFrame(1, bridge::Date{1071, 10, 3}, 312);
}

bridge::Frame secondFrame() {
    return baseFrame(2, bridge::Date{1071, 11, 14}, 298);
}

std::string firstAnswer() {
    // Ælfwynn Testdóttir, Ярослав Тестов, Muḥammad ibn Test; the long word mixes þ, ð, ö and ü.
    return "## Your realm today\n"
           "Your treasury holds **312 gold** and grows by about +7.8 a month. Your heir, "
           "**\xC3\x86lfwynn Testd\xC3\xB3ttir**, likes you.\n"
           "\n"
           "1. **Win over \xD0\xAF\xD1\x80\xD0\xBE\xD1\x81\xD0\xBB\xD0\xB0\xD0\xB2 "
           "\xD0\xA2\xD0\xB5\xD1\x81\xD1\x82\xD0\xBE\xD0\xB2**: your strongest vassal dislikes "
           "you (opinion -12). Open the `Vassals` window and send a gift.\n"
           "2. **Watch Mu\xE1\xB8\xA5"
           "ammad ibn Test**: they lead a faction against you, at 61% "
           "of the power it needs.\n"
           "3. **Keep 100 gold in reserve**: a war or a revolt can come quickly.\n"
           "\n"
           "- Your claim on Testmark\xC3\xBEr\xC3\xBA\xC3\xB0vangar\xC3\xB6verlandsf\xC3\xBCrsten"
           "t\xC3\xBCmerschaftsgemeindeverwaltungsbezirk is weak.";
}

std::string secondAnswerStart() {
    return "Two groups could turn on you. **Mu\xE1\xB8\xA5"
           "ammad ibn Test** leads ";
}

providers::ChatError secondAnswerError() {
    providers::ChatError error;
    error.code = providers::ChatError::Code::Overloaded;
    error.message = "overloaded (scripted)";
    error.retryable = true;
    error.httpStatus = 529;
    return error;
}

std::vector<std::string> splitIntoPieces(std::string_view text, std::size_t maxBytes) {
    std::vector<std::string> pieces;
    std::size_t at = 0;
    while (at < text.size()) {
        std::size_t end = nextCodePoint(text, at);
        while (end < text.size()) {
            const std::size_t next = nextCodePoint(text, end);
            if (next - at > maxBytes) {
                break;
            }
            end = next;
        }
        pieces.emplace_back(text.substr(at, end - at));
        at = end;
    }
    return pieces;
}

std::vector<ScriptedProvider::Answer> smokeScript() {
    std::vector<ScriptedProvider::Answer> script(2);
    script[0].pieces = splitIntoPieces(firstAnswer(), 9);
    script[0].usage.input = 1200;
    script[0].usage.output = 160;
    script[0].usage.stopReason = "end_turn";
    script[1].pieces = splitIntoPieces(secondAnswerStart(), 9);
    script[1].error = secondAnswerError();
    return script;
}

ScriptedProvider::ScriptedProvider(std::vector<Answer> answers) : answers_(std::move(answers)) {}

void ScriptedProvider::streamChat(const providers::ChatRequest& /*request*/,
                                  providers::StreamHandlers handlers,
                                  providers::CancelToken cancel) {
    Active active;
    active.handlers = std::move(handlers);
    active.cancel = cancel;
    if (nextAnswer_ < answers_.size()) {
        active.answer = nextAnswer_;
    }
    ++nextAnswer_;
    ++requests_;
    active_ = std::move(active);
}

void ScriptedProvider::finish(const std::optional<providers::ChatError>& error,
                              const providers::Usage& usage) {
    if (!active_.has_value()) {
        return;
    }
    // The request is over before its last handler runs, as with a real provider.
    const Active active = std::move(*active_);
    active_.reset();
    if (error.has_value()) {
        providers::ChatError ended = *error;
        ended.partialText = active.sent;
        active.handlers.onError(ended);
    } else {
        active.handlers.onDone(usage);
    }
}

bool ScriptedProvider::step() {
    if (!active_.has_value()) {
        return false;
    }
    Active& active = *active_;
    if (active.cancel.cancelled()) {
        providers::ChatError cancelled;
        cancelled.code = providers::ChatError::Code::Cancelled;
        cancelled.message = "cancelled";
        finish(cancelled, {});
        return true;
    }
    if (!active.answer.has_value()) {
        providers::ChatError none;
        none.code = providers::ChatError::Code::Other;
        none.message = "no scripted answer left";
        finish(none, {});
        return true;
    }
    const Answer& answer = answers_[*active.answer];
    if (active.nextPiece < answer.pieces.size()) {
        if (active.nextPiece == 0 && active.handlers.onStatus) {
            active.handlers.onStatus(providers::StreamStatus::TextStarted);
        }
        const std::string& piece = answer.pieces[active.nextPiece];
        ++active.nextPiece;
        active.sent += piece;
        active.handlers.onToken(providers::TokenKind::Text, piece);
        return true;
    }
    finish(answer.error, answer.usage);
    return true;
}

}  // namespace curia::chatview::canned
