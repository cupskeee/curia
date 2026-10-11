// SPDX-License-Identifier: MIT
#pragma once

#include <functional>
#include <memory>

namespace curia::providers {

// A copyable handle to shared cancellation state: the UI keeps one copy, the provider's reader
// thread another. Every method is thread-safe. The class throws nothing itself.
//
// cancel() sets the flag and then calls the waker, which is how a blocked network wait is
// interrupted. The waker is called at most once per installation, always outside the token's lock
// (so it may call back into the token), and it must not throw.
//
// setWaker() does not wait for a waker call that is already running on another thread: cancel()
// calls a copy of the waker, so that call can still be in progress after setWaker({}) or a
// replacing setWaker() has returned. Whatever a waker captures must therefore stay valid until
// the token itself is released, or until the owner knows no cancel() is in flight (a shared
// pointer or a thread-safe wake handle, not a raw pointer to something that is destroyed early).
class CancelToken {
public:
    CancelToken();
    CancelToken(const CancelToken&) = default;
    CancelToken& operator=(const CancelToken&) = default;
    // No move operations on purpose: a moved-from token would have no state; a move copies.
    ~CancelToken() = default;

    // Idempotent. Only the call that sets the flag can run the waker; later calls return at once.
    void cancel();

    bool cancelled() const noexcept;

    // Installs the waker, replacing any earlier one. If the token is already cancelled the new
    // waker is called once, now, on this thread; otherwise it is called by cancel(). An empty
    // function removes the waker.
    void setWaker(std::function<void()> waker);

private:
    struct State;
    std::shared_ptr<State> state_;
};

}  // namespace curia::providers
