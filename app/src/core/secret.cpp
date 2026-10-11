// SPDX-License-Identifier: MIT
// memset_s is declared by <string.h> only when this is defined before the first include.
#if defined(__APPLE__)
#define __STDC_WANT_LIB_EXT1__ 1
#endif

#include "core/secret.h"

#include <cstring>
#include <utility>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace curia {

void secureZero(void* data, std::size_t size) noexcept {
    if (data == nullptr || size == 0) {
        return;
    }
#if defined(_WIN32)
    SecureZeroMemory(data, size);
#elif defined(__APPLE__)
    (void)memset_s(data, size, 0, size);
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 25))
    explicit_bzero(data, size);
#else
    // Portable fallback: writes through a volatile pointer may not be removed or merged.
    volatile unsigned char* bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
#endif
}

void wipe(std::string& text) noexcept {
    // resize() up to the capacity cannot reallocate, so the whole buffer (including bytes left
    // behind by an earlier, longer value) is covered without touching memory outside the string.
    text.resize(text.capacity());
    secureZero(text.data(), text.size());
    text.clear();
}

SecretString::SecretString(std::string&& value) : size_(value.size()) {
    if (size_ != 0) {
        data_ = std::make_unique<char[]>(size_);
        std::memcpy(data_.get(), value.data(), size_);
    }
    wipe(value);
}

SecretString::SecretString(SecretString&& other) noexcept
    : data_(std::move(other.data_)), size_(std::exchange(other.size_, 0)) {}

SecretString& SecretString::operator=(SecretString&& other) noexcept {
    if (this != &other) {
        secureZero(data_.get(), size_);
        data_ = std::move(other.data_);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

SecretString::~SecretString() {
    secureZero(data_.get(), size_);
}

std::string_view SecretString::reveal() const noexcept {
    return {data_.get(), size_};
}

bool SecretString::contains(std::string_view needle) const noexcept {
    return reveal().find(needle) != std::string_view::npos;
}

}  // namespace curia
