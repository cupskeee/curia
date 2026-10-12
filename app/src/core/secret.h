// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>
#include <string_view>

namespace curia {

// Overwrites a buffer with zeros in a way the optimiser may not drop (SecureZeroMemory on Windows,
// memset_s on macOS, explicit_bzero on glibc, a volatile loop elsewhere). Best effort: it cannot
// reach copies the program made earlier (a std::string that grew and reallocated, a register, a
// swapped page, a core dump), it only shortens the time a secret stays readable in the buffers
// that Curia owns.
void secureZero(void* data, std::size_t size) noexcept;

// Zeroes the whole capacity of the string (not only the size: a shrunk string keeps old bytes
// past its end) and empties it.
void wipe(std::string& text) noexcept;

// An API key (or any other secret) in memory. The type exists so that a secret cannot reach a log
// line, a stream, a format call or a copy by accident: it has no copy operations, no operator<<
// (deleted), no conversion to std::string, std::string_view or bool, and no comparison operators.
//
// The only way to read the bytes is reveal(). Only the transport (app/src/http/http_client.*, the
// one place that builds the header line) may call it; tools/ci/check_source_rules.sh fails the
// build of the repository on any other call outside the allowlist there. Everything else asks
// narrow questions (size(), empty(), contains()) or hands the SecretString to
// SecretRegistry::add().
//
// The bytes live in a private heap buffer that is zeroed (best effort, see secureZero) when the
// object is destroyed or assigned over. A moved-from object is empty.
class SecretString {
public:
    SecretString() noexcept = default;
    // Takes the bytes of `value` and wipes the source (capacity included) and empties it, so no
    // second copy stays behind in the caller's string. Pass the string with std::move.
    explicit SecretString(std::string&& value);
    // A copy would leave a stray std::string in the caller: always std::move.
    explicit SecretString(const std::string& value) = delete;

    SecretString(const SecretString&) = delete;
    SecretString& operator=(const SecretString&) = delete;
    SecretString(SecretString&& other) noexcept;
    SecretString& operator=(SecretString&& other) noexcept;
    ~SecretString();

    // The secret bytes, valid until this object is modified or destroyed. Transport only.
    std::string_view reveal() const noexcept;

    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // True if the secret contains `needle`. Lets policy code ask a yes/no question (for example
    // "is this a test key?") without reading the bytes.
    bool contains(std::string_view needle) const noexcept;

    // Streaming a secret does not compile.
    friend std::ostream& operator<<(std::ostream&, const SecretString&) = delete;

private:
    std::unique_ptr<char[]> data_;
    std::size_t size_ = 0;
};

}  // namespace curia
