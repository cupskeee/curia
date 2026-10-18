// SPDX-License-Identifier: MIT
#include "core/game_poll.h"

#include <utility>

namespace curia {

GamePoll::GamePoll(Query query, int everyTicks)
    : query_(std::move(query)), everyTicks_(everyTicks < 1 ? 1 : everyTicks) {}

std::optional<bool> GamePoll::onTick() {
    if (ticksUntilQuery_ > 0) {
        --ticksUntilQuery_;
        return std::nullopt;
    }
    ticksUntilQuery_ = everyTicks_ - 1;
    if (!query_) {
        return std::nullopt;
    }
    ++queries_;
    const std::optional<bool> answer = query_();
    if (!answer.has_value() || answer == last_) {
        return std::nullopt;
    }
    last_ = answer;
    return answer;
}

}  // namespace curia
