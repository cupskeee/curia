// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "core/secret.h"

namespace curia {

// What redact() puts in place of a secret.
inline constexpr std::string_view kRedactedMarker = "[redacted]";

namespace detail {
class SecretMatcher;  // multi-pattern matcher, defined in redact.cpp
}

// The set of secrets this process must never write anywhere, and the function that scrubs text of
// them. Every line of the app log passes through redact() (architecture.md 5.4: the app log never
// records keys), and so do error messages built from a server reply.
//
// redact() removes, in this order of concern:
//   (a) every registered secret, wherever it occurs (inside JSON, after "Bearer ", in a URL); the
//       bytes must be contiguous;
//   (b) anything shaped like an Anthropic key, "sk-ant-" followed by 16 or more of [A-Za-z0-9_-],
//       even if it was never registered;
//   (c) the value of a header-like "authorization:", "x-api-key:" or "proxy-authorization:" (name
//       in any letter case, optional blanks or one quote before the colon, blanks after it) up to
//       the end of the line, and the token after "Bearer " (the token ends at a blank, a control
//       byte or a quote).
// Each covered run of bytes becomes one kRedactedMarker; the header name, the colon and the blanks
// stay, so the line remains readable.
//
// Limits of (b) and (c), which are shapes and so only a net for tokens that were never registered:
// the "sk-ant-" prefix is lower-case only; "authorization=tok", "Bearer: tok", a JSON-escaped
// \"Authorization\":\"...\", "x_api_key:", "api-key:", "Cookie:" and a header folded onto the next
// line are not recognised. Only (a) is a guarantee, and only for secrets that were registered with
// add(). So the callers must register the key first, and must never pass a raw header dump or a
// request or reply body to the log: the AppLog API takes only short scalar fields for that reason.
//
// Properties, all tested:
//   * Linear time. Registered secrets are matched with an Aho-Corasick automaton (one pass over the
//     text whatever the number of secrets, so a long input with many secrets that share prefixes
//     does not go quadratic); the three pattern families are scanned once each with cached line
//     and token ends so repeated keywords cannot rescan the same bytes; covered runs are merged
//     with a stack in amortised constant time per run. The registry is bounded (kMaxSecrets,
//     kMaxSecretBytes) so building the automaton is trivial and its size is fixed.
//   * Works on bytes: invalid UTF-8, NUL bytes and very long input are handled without reading out
//     of bounds. It allocates (a std::bad_alloc ends the process, as everywhere in the app) but
//     never throws on its own.
//   * Idempotent: redact(redact(x)) == redact(x). Replacing a run can in rare cases create a new
//     match (a secret ending in '[' next to the marker), so the scan repeats until the text no
//     longer changes; after kMaxRounds unstable rounds the whole result is replaced by the marker,
//     which is stable. add() refuses secrets that are a part of the marker for the same reason.
//   * Overlapping, adjacent and nested secrets are all covered; "longest first" falls out of
//     taking the union of every occurrence.
//
// Thread-safe. redact() takes a snapshot of the matcher under the lock and scans without it, so a
// concurrent add() never delays a log line for more than a pointer copy.
class SecretRegistry {
public:
    // Shorter secrets are refused: they would match innocent text everywhere, and no real key is
    // that short.
    static constexpr std::size_t kMinSecretBytes = 8;
    static constexpr std::size_t kMaxSecretBytes = 4096;
    static constexpr std::size_t kMaxSecrets = 16;
    static constexpr int kMaxRounds = 8;

    SecretRegistry();
    ~SecretRegistry();
    SecretRegistry(const SecretRegistry&) = delete;
    SecretRegistry& operator=(const SecretRegistry&) = delete;

    // The process-wide registry used by redact() and the app log. Never destroyed (so a log call
    // during static destruction is safe).
    static SecretRegistry& instance();

    // Registers a secret. Returns false and registers nothing if it is shorter than
    // kMinSecretBytes, longer than kMaxSecretBytes, a part of kRedactedMarker, or the registry
    // already holds kMaxSecrets others. Registering the same secret again returns true.
    bool add(std::string_view secret);
    bool add(const SecretString& secret);

    // Forgets a secret (its bytes are zeroed). Returns whether it was registered.
    bool remove(std::string_view secret);
    // Forgets all secrets.
    void clear();

    std::size_t size() const;

    std::string redact(std::string_view text) const;

private:
    void rebuildLocked();

    mutable std::mutex mutex_;
    std::vector<std::string> secrets_;
    std::shared_ptr<const detail::SecretMatcher> matcher_;
};

// redact() with the process-wide registry.
std::string redact(std::string_view text);

}  // namespace curia
