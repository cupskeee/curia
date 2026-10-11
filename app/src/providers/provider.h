// SPDX-License-Identifier: MIT
#pragma once

#include "providers/cancel_token.h"
#include "providers/chat_types.h"

namespace curia::providers {

// A chat backend. listModels() and caps() join this interface in M5 (docs/architecture.md
// section 6); M4 needs the streaming call only.
class Provider {
public:
    Provider() = default;
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;
    virtual ~Provider() = default;

    // Starts the request and returns at once; the answer arrives through `handlers` on the
    // provider's own thread. Exactly one of onDone and onError is called, once. If `cancel` is
    // cancelled the request ends with ChatError::Code::Cancelled, with the text received so far in
    // partialText. Failures are reported through onError, never thrown.
    virtual void streamChat(const ChatRequest& request, StreamHandlers handlers,
                            CancelToken cancel) = 0;
};

}  // namespace curia::providers
