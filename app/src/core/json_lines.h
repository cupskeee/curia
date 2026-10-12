// SPDX-License-Identifier: MIT
#pragma once

#include <mutex>
#include <ostream>
#include <string>

#include <nlohmann/json.hpp>

#include "core/redact.h"

namespace curia {

// The output side of the headless modes: one JSON object per line, flushed per line.
//
// What a line looks like, always:
//   * one physical line, ending in '\n' (a control character in a text is escaped, so a text can
//     never start another line);
//   * pure ASCII: every non-ASCII character is written as a \uXXXX escape, so the output is the
//     same on every console code page and cannot be split by a reader that treats U+2028 or NEL
//     as a line end;
//   * valid: bytes that are not valid UTF-8 become U+FFFD instead of making the writer throw;
//   * scrubbed: every string VALUE (a nested one too) passes through the registry's redact()
//     before it is written, so a registered secret, a key-shaped token or an authorization header
//     in a text never reaches the output. This is the same function the chat view and the app log
//     use. Names of keys are the program's own words and are not scrubbed.
//
// Thread-safe: write() may be called from any thread; lines never interleave.
class JsonLineWriter {
public:
    // `registry` null = the process-wide registry. Not owned; must outlive the writer.
    explicit JsonLineWriter(std::ostream& out, const SecretRegistry* registry = nullptr);

    JsonLineWriter(const JsonLineWriter&) = delete;
    JsonLineWriter& operator=(const JsonLineWriter&) = delete;

    void write(nlohmann::json event);

    // The line write() would produce, without the newline (pure; for the tests).
    static std::string format(nlohmann::json event, const SecretRegistry* registry = nullptr);

private:
    std::ostream& out_;
    const SecretRegistry* registry_;
    std::mutex mutex_;
};

// Replaces every string value in `value` (recursively) by its redacted form.
void redactJsonStrings(nlohmann::json& value, const SecretRegistry* registry);

}  // namespace curia
