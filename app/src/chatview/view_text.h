// SPDX-License-Identifier: MIT
#pragma once

#include <array>
#include <string>
#include <string_view>

#include "bridge/frame.h"

// Every text the chat window shows of its own, in one place. All of them follow the plain-words
// rule (FR-OVL-11, advisor/plain_words.h; a unit test checks each one): the player reads "what I
// can see", "an update from the game", "click Advisor", never Curia's internal words.
//
// Texts that come from elsewhere and are reused as they are: the error summaries
// (advisor/chat_error_text.h, advisor::kEmptyAnswerText, advisor::kNoRoomForAnswerText), the
// divider rows (curia::dividerLabel(), core/app_controller.h) and the not-configured message
// (curia::kDefaultNotConfigured, or describeNotConfigured() of core/dev_config.h, which names the
// missing variables of a development build and never a value).
namespace curia::chatview::text {

// Nothing has come from the game since Curia started: in the middle of the empty conversation, or
// in the status line once the conversation has rows. (The Advisor button's place on the game
// screen is not named: it may still move.)
inline constexpr std::string_view kNothingReadYet =
    "I can't see your realm yet. Pause CK3, then click the Advisor button in the game.";
// seenFrom(): "What I can see is from 3 October 1071."
inline constexpr std::string_view kSeenFromStart = "What I can see is from ";
inline constexpr std::string_view kSeenFromEnd = ".";
// No CK3 user folder (with its logs folder) was found, so nothing is read. On Windows and macOS
// the folder is in Documents; elsewhere (Linux, Proton) it can be in several places.
inline constexpr std::string_view kFolderNotFoundDocuments =
    "Curia can't find CK3's folder in Documents \xE2\x80\xBA Paradox Interactive \xE2\x80\xBA "
    "Crusader Kings III. Start CK3 once, then restart Curia.";
inline constexpr std::string_view kFolderNotFound =
    "Curia can't find CK3's folder (Crusader Kings III, inside Paradox Interactive). Start CK3 "
    "once, then restart Curia.";

// Notices under the status line.
inline constexpr std::string_view kUnreadable =
    "The last update from the game could not be read. Click Advisor again.";
// The mod's "Curia probes" button (a test button, mod/curia/localization).
inline constexpr std::string_view kProbe =
    "The last update came from the test button (Curia probes). Click Advisor instead.";
inline constexpr std::string_view kVersionMismatch =
    "The Curia mod in the game and this app are different versions. Update both to the same "
    "version.";
// The log-silence notices (advisor::Notice); the first one hedges, because a healthy CK3 writes
// nothing until the player clicks Advisor (advisor/README.md).
inline constexpr std::string_view kRestartCk3 =
    "No update from the game yet. Click Advisor in the game; if that does nothing, restart CK3.";
inline constexpr std::string_view kSilentSoft =
    "Nothing new has come from the game for a while. If clicking Advisor does nothing, restart "
    "CK3.";
inline constexpr std::string_view kCk3NotRunning =
    "CK3 is not running. Start it, then click Advisor in the game.";

// Next to the input box (the pause reminder is FR-OVL-10): the lead in bold ink, the rest muted.
// It names CK3's default pause key (owner, 2026-10-18).
inline constexpr std::string_view kPauseReminderLead = "Press Space";
inline constexpr std::string_view kPauseReminderRest =
    " to pause first: the game keeps running while you type.";
inline constexpr std::string_view kInputHint =
    "Ask your advisor (Enter sends, Shift+Enter adds a line)";

// The conversation.
inline constexpr std::string_view kYou = "You";
inline constexpr std::string_view kAdvisor = "Advisor";
// While the question waits for the first word of its answer (FR-OVL-12).
inline constexpr std::string_view kThinking = "thinking...";
inline constexpr std::string_view kStopped = "Stopped.";
// The answer reached the provider's length limit (a normal end that the player should see).
inline constexpr std::string_view kCutShort =
    "The answer was cut short because it reached its length limit. Ask for a shorter answer, or "
    "ask again.";

// Buttons.
inline constexpr std::string_view kSend = "Send";
inline constexpr std::string_view kStop = "Stop";
inline constexpr std::string_view kRetry = "Retry";
inline constexpr std::string_view kCopy = "Copy";
inline constexpr std::string_view kCopied = "Copied";

// The tray menu (FR-OVL-6; the menu bar on macOS, the notification area on Windows): its tooltip
// and its three entries.
inline constexpr std::string_view kTrayTooltip = "Curia";
inline constexpr std::string_view kTrayShow = "Show Curia";
inline constexpr std::string_view kTrayHide = "Hide Curia";
inline constexpr std::string_view kTrayQuit = "Quit Curia";

// "What I can see is from 3 October 1071."
std::string seenFrom(const bridge::Date& date);

// Every fixed text above (the plain-words test walks them; the smoke test checks the font has
// every character of them).
inline constexpr std::array<std::string_view, 28> kAllFixedTexts = {
    kNothingReadYet,
    kSeenFromStart,
    kSeenFromEnd,
    kFolderNotFoundDocuments,
    kFolderNotFound,
    kUnreadable,
    kProbe,
    kVersionMismatch,
    kRestartCk3,
    kSilentSoft,
    kCk3NotRunning,
    kPauseReminderLead,
    kPauseReminderRest,
    kInputHint,
    kYou,
    kAdvisor,
    kThinking,
    kStopped,
    kCutShort,
    kSend,
    kStop,
    kRetry,
    kCopy,
    kCopied,
    kTrayTooltip,
    kTrayShow,
    kTrayHide,
    kTrayQuit,
};

}  // namespace curia::chatview::text
