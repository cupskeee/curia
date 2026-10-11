// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <cstring>
#include <ostream>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "core/secret.h"

using curia::SecretString;

namespace {

// A key-shaped value built at run time, never written out as one literal.
std::string fakeKey() {
    return std::string("sk-ant-") + "FAKE" + "0123456789abcdef";
}

template <typename T>
concept Streamable = requires(std::ostream& os, const T& value) { os << value; };

}  // namespace

TEST_CASE("secret: the type cannot be copied, converted or streamed") {
    static_assert(!std::is_copy_constructible_v<SecretString>);
    static_assert(!std::is_copy_assignable_v<SecretString>);
    static_assert(std::is_nothrow_move_constructible_v<SecretString>);
    static_assert(std::is_nothrow_move_assignable_v<SecretString>);
    static_assert(!std::is_convertible_v<SecretString, std::string>);
    static_assert(!std::is_convertible_v<SecretString, std::string_view>);
    static_assert(!std::is_convertible_v<SecretString, bool>);
    static_assert(!std::is_convertible_v<std::string, SecretString>);
    static_assert(
        !std::is_convertible_v<std::string&&, SecretString>);  // the constructor is explicit
    static_assert(!std::is_constructible_v<SecretString, const std::string&>);
    static_assert(!Streamable<SecretString>);
    CHECK(true);
}

TEST_CASE("secret: construction moves the bytes in and clears the source") {
    std::string source = fakeKey();
    const std::string expected = source;
    SecretString secret(std::move(source));
    CHECK(secret.reveal() == expected);
    CHECK(secret.size() == expected.size());
    CHECK_FALSE(secret.empty());
    CHECK(source.empty());  // NOLINT(bugprone-use-after-move): the documented post-state
}

TEST_CASE("secret: a long source is wiped too") {
    std::string source(500, 'q');
    SecretString secret(std::move(source));
    CHECK(secret.size() == 500);
    CHECK(source.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_CASE("secret: default and empty") {
    const SecretString none;
    CHECK(none.empty());
    CHECK(none.size() == 0);
    CHECK(none.reveal().empty());
    CHECK_FALSE(none.contains("x"));
    CHECK(none.contains(""));

    SecretString fromEmpty{std::string()};
    CHECK(fromEmpty.empty());
}

TEST_CASE("secret: moves leave the source empty") {
    SecretString first(fakeKey());
    SecretString second(std::move(first));
    CHECK(second.reveal() == fakeKey());
    CHECK(first.empty());  // NOLINT(bugprone-use-after-move): moved-from state is documented

    SecretString third(std::string("another-secret-value"));
    third = std::move(second);
    CHECK(third.reveal() == fakeKey());
    CHECK(second.empty());  // NOLINT(bugprone-use-after-move)

    // Self-move-assignment keeps the value.
    SecretString& alias = third;
    third = std::move(alias);
    CHECK(third.reveal() == fakeKey());
}

TEST_CASE("secret: contains answers yes or no without exposing the bytes") {
    const SecretString secret(fakeKey());
    CHECK(secret.contains("FAKE"));
    CHECK_FALSE(secret.contains("fake"));
    CHECK_FALSE(secret.contains("REAL"));
    CHECK(secret.contains("sk-ant-"));
}

TEST_CASE("secret: secureZero and wipe") {
    unsigned char buffer[32];
    std::memset(buffer, 0xAB, sizeof buffer);
    curia::secureZero(buffer, sizeof buffer);
    for (const unsigned char byte : buffer) {
        CHECK(byte == 0);
    }
    curia::secureZero(nullptr, 10);  // no crash
    curia::secureZero(buffer, 0);

    std::string text = "some secret text that is longer than the small-string buffer";
    curia::wipe(text);
    CHECK(text.empty());
}

TEST_CASE("secret: embedded NUL and non-ASCII bytes survive") {
    std::string raw("ab\0cd\xff\xfe", 7);
    const std::string copy = raw;
    const SecretString secret(std::move(raw));
    CHECK(secret.reveal() == std::string_view(copy));
    CHECK(secret.size() == 7);
}
