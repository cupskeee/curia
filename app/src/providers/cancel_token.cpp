// SPDX-License-Identifier: MIT
#include "providers/cancel_token.h"

#include <atomic>
#include <mutex>
#include <utility>

namespace curia::providers {

struct CancelToken::State {
    std::atomic<bool> cancelled{false};
    // Guards waker and delivered.
    std::mutex mutex;
    std::function<void()> waker;
    // The installed waker has been called (or is being called) by whichever side saw the
    // cancellation first; keeps it to once even when cancel() and setWaker() race.
    bool delivered = false;
};

CancelToken::CancelToken() : state_(std::make_shared<State>()) {}

void CancelToken::cancel() {
    if (state_->cancelled.exchange(true)) {
        return;
    }
    std::function<void()> waker;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        if (!state_->delivered) {
            state_->delivered = true;
            waker = state_->waker;
        }
    }
    if (waker) {
        waker();
    }
}

bool CancelToken::cancelled() const noexcept {
    return state_->cancelled.load();
}

void CancelToken::setWaker(std::function<void()> waker) {
    std::function<void()> callNow;
    {
        const std::lock_guard<std::mutex> lock(state_->mutex);
        state_->waker = std::move(waker);
        state_->delivered = state_->cancelled.load();
        if (state_->delivered) {
            callNow = state_->waker;
        }
    }
    if (callNow) {
        callNow();
    }
}

}  // namespace curia::providers
