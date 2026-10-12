// SPDX-License-Identifier: MIT
#pragma once

#include "advisor/transcript.h"
#include "core/redact.h"
#include "providers/chat_types.h"

namespace curia::advisor {

// The player-facing description of a failed request. The summary is chosen here from the error
// code alone (no server text in it), so it cannot carry a secret or markup. The detail is the
// provider's own message, passed through the registry's redact() once more (null = the
// process-wide registry) and cut to kMaxErrorDetailBytes at a character boundary; it is meant for
// a "details" line, not for the headline.
//   Auth        the key was rejected                  Network     could not reach the service
//   RateLimit   too many requests (with the wait)     Overloaded  the service is busy
//   BadRequest  the request was refused (model name)  Policy      the service declined to answer
//   Cancelled   stopped                               Other       a problem on the service side
inline constexpr std::size_t kMaxErrorDetailBytes = 300;

ErrorInfo describeChatError(const providers::ChatError& error,
                            const SecretRegistry* registry = nullptr);

}  // namespace curia::advisor
