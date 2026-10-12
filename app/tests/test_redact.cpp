// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/redact.h"

using curia::kRedactedMarker;
using curia::SecretRegistry;

namespace {

const std::string kMark(kRedactedMarker);

// Key-shaped values are built from parts at run time, never written out as one literal.
std::string fakeKey(const std::string& tail = "0123456789abcdef") {
    return std::string("sk-ant-") + "FAKE" + tail;
}

// A registry holding the given secrets (all of them must be accepted).
struct Fixture {
    SecretRegistry registry;
    explicit Fixture(const std::vector<std::string>& secrets = {}) {
        for (const std::string& secret : secrets) {
            REQUIRE(registry.add(secret));
        }
    }
};

// --- an independent, deliberately naive reference
// --------------------------------------------------- Quadratic and obvious: it marks covered bytes
// in a bitmap by testing every position against every pattern, the way the specification is
// written. Used to check the fast scanner on random input.

bool isBlankNaive(char c) {
    return c == ' ' || c == '\t';
}

char lowerNaive(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

bool wordAt(const std::string& text, std::size_t at, const std::string& lowerWord) {
    if (at + lowerWord.size() > text.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lowerWord.size(); ++i) {
        if (lowerNaive(text[at + i]) != lowerWord[i]) {
            return false;
        }
    }
    return true;
}

bool keyCharNaive(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           c == '-';
}

// One round; returns whether any byte was covered.
bool naiveRound(const std::string& text, const std::vector<std::string>& secrets,
                std::string& out) {
    const std::size_t n = text.size();
    std::vector<bool> cover(n, false);
    bool any = false;
    const auto mark = [&](std::size_t from, std::size_t to) {
        for (std::size_t k = from; k < to; ++k) {
            cover[k] = true;
            any = true;
        }
    };
    for (const std::string& secret : secrets) {
        for (std::size_t i = 0; i + secret.size() <= n; ++i) {
            if (text.compare(i, secret.size(), secret) == 0) {
                mark(i, i + secret.size());
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (text.compare(i, 7, "sk-ant-") == 0) {
            std::size_t j = i + 7;
            while (j < n && keyCharNaive(text[j])) {
                ++j;
            }
            if (j - (i + 7) >= 16) {
                mark(i, j);
            }
        }
    }
    for (const std::string name : {"authorization", "x-api-key", "proxy-authorization"}) {
        for (std::size_t i = 0; i < n; ++i) {
            if (!wordAt(text, i, name)) {
                continue;
            }
            std::size_t p = i + name.size();
            while (p < n && isBlankNaive(text[p])) {
                ++p;
            }
            if (p < n && (text[p] == '"' || text[p] == '\'')) {
                ++p;
                while (p < n && isBlankNaive(text[p])) {
                    ++p;
                }
            }
            if (p >= n || text[p] != ':') {
                continue;
            }
            ++p;
            while (p < n && isBlankNaive(text[p])) {
                ++p;
            }
            std::size_t e = p;
            while (e < n && text[e] != '\n' && text[e] != '\r') {
                ++e;
            }
            mark(p, e);
        }
    }
    for (std::size_t i = 0; i < n; ++i) {
        if (!wordAt(text, i, "bearer")) {
            continue;
        }
        std::size_t p = i + 6;
        if (p >= n || !isBlankNaive(text[p])) {
            continue;
        }
        while (p < n && isBlankNaive(text[p])) {
            ++p;
        }
        std::size_t e = p;
        while (e < n) {
            const auto b = static_cast<unsigned char>(text[e]);
            if (b <= 0x20 || b == 0x7f || text[e] == '"' || text[e] == '\'') {
                break;
            }
            ++e;
        }
        mark(p, e);
    }
    if (!any) {
        return false;
    }
    out.clear();
    for (std::size_t i = 0; i < n;) {
        if (!cover[i]) {
            out.push_back(text[i++]);
            continue;
        }
        while (i < n && cover[i]) {
            ++i;
        }
        out += kMark;
    }
    return true;
}

std::string naiveRedact(const std::string& text, const std::vector<std::string>& secrets) {
    std::string current = text;
    for (int round = 0; round < SecretRegistry::kMaxRounds; ++round) {
        std::string next;
        if (!naiveRound(current, secrets, next) || next == current) {
            return current;
        }
        current = next;
    }
    return kMark;
}

}  // namespace

// --- registry rules -----------------------------------------------------------------------------

TEST_CASE("redact: add refuses short, long and marker-shaped secrets") {
    SecretRegistry registry;
    CHECK_FALSE(registry.add(""));
    CHECK_FALSE(registry.add("1234567"));  // 7 bytes
    CHECK(registry.size() == 0);
    CHECK(registry.add("12345678"));  // 8 bytes
    CHECK(registry.size() == 1);
    CHECK_FALSE(registry.add(std::string(SecretRegistry::kMaxSecretBytes + 1, 'k')));
    CHECK(registry.add(std::string(SecretRegistry::kMaxSecretBytes, 'k')));
    // A part of the marker would be found again in the marker itself.
    CHECK_FALSE(registry.add("redacted"));
    CHECK_FALSE(registry.add("[redacted"));
    CHECK_FALSE(registry.add("redacted]"));
    CHECK_FALSE(registry.add("[redacted]"));
    CHECK(registry.size() == 2);
}

TEST_CASE("redact: duplicates, removal, clear and the size bound") {
    SecretRegistry registry;
    CHECK(registry.add("duplicate-secret"));
    CHECK(registry.add("duplicate-secret"));
    CHECK(registry.size() == 1);
    CHECK_FALSE(registry.remove("not-registered-at-all"));
    CHECK(registry.remove("duplicate-secret"));
    CHECK(registry.size() == 0);
    CHECK(registry.redact("duplicate-secret") == "duplicate-secret");  // forgotten

    for (std::size_t i = 0; i < SecretRegistry::kMaxSecrets; ++i) {
        CHECK(registry.add("registered-secret-" + std::to_string(i)));
    }
    CHECK_FALSE(registry.add("one-too-many-secrets"));
    CHECK(registry.size() == SecretRegistry::kMaxSecrets);
    CHECK(registry.add("registered-secret-3"));  // already there: still fine
    registry.clear();
    CHECK(registry.size() == 0);
    CHECK(registry.add("one-too-many-secrets"));
}

TEST_CASE("redact: add accepts a SecretString") {
    SecretRegistry registry;
    const curia::SecretString secret(std::string("secret-from-a-wrapper"));
    CHECK(registry.add(secret));
    CHECK(registry.redact("x secret-from-a-wrapper y") == "x " + kMark + " y");
    const curia::SecretString shortSecret(std::string("short"));
    CHECK_FALSE(registry.add(shortSecret));
}

// --- registered secrets
// ---------------------------------------------------------------------------

TEST_CASE("redact: a registered secret at the start, middle, end and alone") {
    Fixture f({"hunter2-hunter2"});
    CHECK(f.registry.redact("hunter2-hunter2 tail") == kMark + " tail");
    CHECK(f.registry.redact("head hunter2-hunter2 tail") == "head " + kMark + " tail");
    CHECK(f.registry.redact("head hunter2-hunter2") == "head " + kMark);
    CHECK(f.registry.redact("hunter2-hunter2") == kMark);
    CHECK(f.registry.redact("") == "");
    CHECK(f.registry.redact("no secret here") == "no secret here");
    CHECK(f.registry.redact("hunter2-hunter") == "hunter2-hunter");  // one byte short
}

TEST_CASE("redact: a registered secret inside JSON, a URL and after Bearer") {
    Fixture f({"hunter2-hunter2"});
    CHECK(f.registry.redact(R"({"key":"hunter2-hunter2","n":1})") ==
          "{\"key\":\"" + kMark + "\",\"n\":1}");
    CHECK(f.registry.redact("https://host/path?k=hunter2-hunter2&x=1") ==
          "https://host/path?k=" + kMark + "&x=1");
    CHECK(f.registry.redact("see Bearer hunter2-hunter2.") == "see Bearer " + kMark);
    CHECK(f.registry.redact("a hunter2-hunter2 b hunter2-hunter2 c") ==
          "a " + kMark + " b " + kMark + " c");
}

TEST_CASE("redact: overlapping, adjacent, nested and prefix-sharing secrets") {
    Fixture f({"abcdefgh", "defghijk", "abcdefghijk", "cdefghij", "qrstuvwx", "qrstuvwxyz"});
    // Overlap: the union of the occurrences is one covered run.
    CHECK(f.registry.redact("abcdefghijk") == kMark);
    CHECK(f.registry.redact("x abcdefgh y") == "x " + kMark + " y");
    CHECK(f.registry.redact("x defghijk y") == "x " + kMark + " y");
    // Substring of another secret: the longer one wins, nothing of it is left over.
    CHECK(f.registry.redact("qrstuvwxyz!") == kMark + "!");
    CHECK(f.registry.redact("qrstuvwx!") == kMark + "!");
    // Adjacent secrets are one covered run, one marker.
    CHECK(f.registry.redact("abcdefghqrstuvwx") == kMark);
    CHECK(f.registry.redact("abcdefgh|qrstuvwx") == kMark + "|" + kMark);
}

TEST_CASE("redact: a secret that ends with a bracket is still gone after one call") {
    Fixture f({"xxxxxxx[", "yyyyyyyy"});
    const std::string once = f.registry.redact("xxxxxxxyyyyyyyy");
    CHECK(once.find("xxxxxxx[") == std::string::npos);
    CHECK(f.registry.redact(once) == once);
}

// --- key shapes and header values
// -----------------------------------------------------------------

TEST_CASE("redact: anything shaped like an Anthropic key goes, registered or not") {
    SecretRegistry registry;
    const std::string tail16(16, 'a');
    CHECK(registry.redact("k=sk-ant-" + tail16 + "!") == "k=" + kMark + "!");
    CHECK(registry.redact("k=sk-ant-" + std::string(15, 'a') + "!") ==
          "k=sk-ant-" + std::string(15, 'a') + "!");
    CHECK(registry.redact("sk-ant-api03-ab_cd-" + tail16) == kMark);
    CHECK(registry.redact("xsk-ant-" + tail16) == "x" + kMark);
    CHECK(registry.redact("sk-ant-" + tail16 + "sk-ant-" + tail16) == kMark);
    CHECK(registry.redact(fakeKey()) == kMark);
    CHECK(registry.redact("SK-ANT-" + tail16) == "SK-ANT-" + tail16);  // case-sensitive prefix
}

TEST_CASE("redact: header values up to the end of the line") {
    SecretRegistry registry;
    CHECK(registry.redact("authorization: abc def\nnext line") ==
          "authorization: " + kMark + "\nnext line");
    CHECK(registry.redact("Authorization:    token-value\r\nnext") ==
          "Authorization:    " + kMark + "\r\nnext");
    CHECK(registry.redact("X-API-KEY:\tvalue with spaces") == "X-API-KEY:\t" + kMark);
    CHECK(registry.redact("x-api-key:value") == "x-api-key:" + kMark);
    CHECK(registry.redact("Proxy-Authorization: Basic abc") == "Proxy-Authorization: " + kMark);
    CHECK(registry.redact("authorization : v") == "authorization : " + kMark);
    CHECK(registry.redact(R"("authorization": "v", "x": 1)") == R"("authorization": )" + kMark);
    CHECK(registry.redact("a=1 authorization: v1\nb=2 x-api-key: v2\n") ==
          "a=1 authorization: " + kMark + "\nb=2 x-api-key: " + kMark + "\n");
    // Nothing after the colon: nothing to hide.
    CHECK(registry.redact("authorization:") == "authorization:");
    CHECK(registry.redact("authorization:   \nx") == "authorization:   \nx");
    // Not a header: no colon.
    CHECK(registry.redact("authorization is required") == "authorization is required");
}

TEST_CASE("redact: the token after Bearer") {
    SecretRegistry registry;
    CHECK(registry.redact("Bearer abc.def-ghi rest") == "Bearer " + kMark + " rest");
    CHECK(registry.redact("bearer   tok") == "bearer   " + kMark);
    CHECK(registry.redact("BEARER\ttok;x") == "BEARER\t" + kMark);
    CHECK(registry.redact(R"({"h":"Bearer tok123"})") == "{\"h\":\"Bearer " + kMark + "\"}");
    CHECK(registry.redact("Bearer") == "Bearer");
    CHECK(registry.redact("Bearer ") == "Bearer ");
    CHECK(registry.redact("Bearer\ntok") == "Bearer\ntok");  // the token must be on the same line
    CHECK(registry.redact("Bearertok") == "Bearertok");
}

// --- robustness
// -------------------------------------------------------------------------------------

TEST_CASE("redact: idempotent on a hand-picked set") {
    Fixture f({"hunter2-hunter2", "xxxxxxx["});
    const std::vector<std::string> inputs = {
        "authorization: Bearer hunter2-hunter2",
        "Bearer " + fakeKey() + " and " + fakeKey("zzzzzzzzzzzzzzzzzz"),
        "x-api-key: " + kMark,
        "Bearer " + kMark + kMark,
        std::string("a\0b ", 4) + "hunter2-hunter2 \xff\xfe",
        "hunter2-hunter2hunter2-hunter2",
        "Bearer ab cd ef\nauthorization: [redacted]",
        "xxxxxxxhunter2-hunter2",
    };
    for (const std::string& input : inputs) {
        const std::string once = f.registry.redact(input);
        CHECK(f.registry.redact(once) == once);
    }
}

TEST_CASE("redact: invalid UTF-8, NUL bytes and every byte value") {
    Fixture f({"hunter2-hunter2"});
    std::string all;
    for (int i = 0; i < 256; ++i) {
        all.push_back(static_cast<char>(i));
    }
    CHECK(f.registry.redact(all) == all);
    const std::string mixed = std::string("\xff\xfe") + std::string(1, '\0') + "hunter2-hunter2" +
                              std::string(1, '\0') + "\xc3";
    CHECK(f.registry.redact(mixed) ==
          std::string("\xff\xfe") + std::string(1, '\0') + kMark + std::string(1, '\0') + "\xc3");
    // A secret with NUL and high bytes inside.
    const std::string odd("ab\0cd\xff\xfe-9", 9);
    REQUIRE(f.registry.add(odd));
    CHECK(f.registry.redact("[" + odd + "]") == "[" + kMark + "]");
}

TEST_CASE("redact: long and adversarial input stays linear") {
    // Each of these is quadratic for a careless scanner: with ~1 MiB per case a quadratic one needs
    // minutes, a linear one well under the generous bound (which also covers sanitizer builds).
    Fixture f({std::string(40, 'a') + "b", std::string(30, 'a') + "c",
               "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab"});
    constexpr std::size_t kSize = 1 << 20;
    const auto start = std::chrono::steady_clock::now();
    std::string text(kSize, 'a');
    CHECK(f.registry.redact(text) == text);  // long partial matches that never complete

    std::string headers;
    while (headers.size() < kSize) {
        headers += "authorization: ";  // all on one line: each value runs to the same line end
    }
    CHECK(f.registry.redact(headers) == "authorization: " + kMark);

    std::string keys;
    while (keys.size() < kSize) {
        keys += "sk-ant-";  // one long run of key characters, every prefix a candidate
    }
    CHECK(f.registry.redact(keys) == kMark);

    std::string bearers;
    while (bearers.size() < kSize) {
        bearers += "bearer ";  // each token is the next keyword
    }
    CHECK_FALSE(f.registry.redact(bearers).empty());

    std::string unfinished = "Bearer ";
    unfinished.append(kSize, 'T');
    CHECK(f.registry.redact(unfinished) == "Bearer " + kMark);

    std::string manySecrets;
    while (manySecrets.size() < kSize) {
        manySecrets += std::string(40, 'a') + "b ";
    }
    CHECK(f.registry.redact(manySecrets).size() < manySecrets.size());
    const auto elapsed = std::chrono::steady_clock::now() - start;
    CHECK(elapsed < std::chrono::seconds(30));
}

TEST_CASE("redact: many registered secrets that share prefixes") {
    SecretRegistry registry;
    const std::string stem(200, 's');
    for (std::size_t i = 0; i < SecretRegistry::kMaxSecrets; ++i) {
        REQUIRE(registry.add(stem + std::string(i + 1, 't')));
    }
    std::string text;
    for (int i = 0; i < 2000; ++i) {
        text += stem + "tt ";
    }
    const std::string out = registry.redact(text);
    CHECK(out.find(stem) == std::string::npos);
}

TEST_CASE("redact: a secret split by the end of input is not matched") {
    Fixture f({"hunter2-hunter2"});
    CHECK(f.registry.redact("hunter2-hunt") == "hunter2-hunt");
}

TEST_CASE("redact: the free function uses the process registry") {
    SecretRegistry& process = SecretRegistry::instance();
    const std::string secret = "process-wide-test-secret";
    REQUIRE(process.add(secret));
    CHECK(curia::redact("a " + secret + " b") == "a " + kMark + " b");
    CHECK(process.remove(secret));
    CHECK(curia::redact("a " + secret + " b") == "a " + secret + " b");
}

// --- the fast scanner against the naive reference
// ----------------------------------------------------

TEST_CASE("redact: random input agrees with the naive reference and holds its promises") {
    const std::vector<std::string> secrets = {"abcdefgh", "cdefghij",     "abcdefghijkl",
                                              "xxxxxxx[", "SECRETSE",     "SECRETSECRET",
                                              "zzzzzzzz", "a b c d e f g"};
    Fixture f(secrets);
    const std::vector<std::string> pieces = {
        "Bearer ",
        "bearer\t",
        "BEARER",
        "authorization: ",
        "Authorization:",
        "X-Api-Key :",
        "x-api-key:\t",
        "proxy-authorization: ",
        "\"authorization\": \"",
        "sk-ant-",
        "sk-ant-ABCDEFGHIJKLMNOP",
        "sk-ant-short",
        "\n",
        "\r\n",
        "\r",
        " ",
        "\t",
        "\"",
        "'",
        "a",
        "b",
        "Z",
        "9",
        "-",
        "_",
        ":",
        std::string(1, '\0'),
        "\xff",
        "[redacted]",
        "[",
        "]",
        "abcd",
        "efgh",
        "ij",
        "kl",
        "xxxx",
        "xxx[",
        "SECRET",
        "SE",
        "zzzz",
        "a b",
        "c d e f g",
        "tok",
        "value",
        ".",
    };
    std::mt19937 rng(20261012);  // fixed seed: the same inputs on every platform
    for (int iteration = 0; iteration < 4000; ++iteration) {
        std::string input;
        const std::size_t count = 1 + rng() % 40;
        for (std::size_t i = 0; i < count; ++i) {
            input += pieces[rng() % pieces.size()];
        }
        const std::string expected = naiveRedact(input, secrets);
        const std::string actual = f.registry.redact(input);
        if (actual != expected) {
            INFO("iteration " << iteration << " input size " << input.size());
            REQUIRE(actual == expected);
        }
        // Idempotent.
        REQUIRE(f.registry.redact(actual) == actual);
        // No registered secret survives.
        for (const std::string& secret : secrets) {
            REQUIRE(actual.find(secret) == std::string::npos);
        }
    }
}

TEST_CASE("redact: automaton with deep failure chains agrees with the naive reference") {
    // Two-letter alphabet: every prefix of every secret is a suffix of some other, which is where a
    // wrong failure link shows. Registered secrets are 8 to 12 bytes long.
    std::mt19937 rng(7);
    for (int round = 0; round < 300; ++round) {
        std::vector<std::string> secrets;
        const std::size_t count = 1 + rng() % 5;
        for (std::size_t i = 0; i < count; ++i) {
            std::string secret;
            const std::size_t length = 8 + rng() % 5;
            for (std::size_t k = 0; k < length; ++k) {
                secret.push_back((rng() & 1U) != 0 ? 'a' : 'b');
            }
            if (std::find(secrets.begin(), secrets.end(), secret) == secrets.end()) {
                secrets.push_back(secret);
            }
        }
        Fixture f(secrets);
        for (int t = 0; t < 40; ++t) {
            std::string text;
            const std::size_t length = rng() % 80;
            for (std::size_t k = 0; k < length; ++k) {
                // Mostly the characters of the secrets, sometimes a separator.
                text.push_back((rng() % 11) == 0 ? '-' : ((rng() & 1U) != 0 ? 'a' : 'b'));
            }
            REQUIRE(f.registry.redact(text) == naiveRedact(text, secrets));
        }
    }
}

// --- threads
// ------------------------------------------------------------------------------------------

TEST_CASE("redact: concurrent add, remove and redact") {
    SecretRegistry registry;
    const std::string stable = "always-registered-secret";
    REQUIRE(registry.add(stable));
    std::atomic<bool> stop{false};
    std::atomic<int> failures{0};

    std::thread writer([&] {
        int i = 0;
        while (!stop.load()) {
            const std::string name = "transient-secret-" + std::to_string(i++ % 8);
            registry.add(name);
            registry.remove(name);
        }
    });
    std::vector<std::thread> readers;
    for (int r = 0; r < 3; ++r) {
        readers.emplace_back([&] {
            const std::string text = "a " + stable + " b " + fakeKey() + " c transient-secret-3 d";
            for (int i = 0; i < 3000; ++i) {
                const std::string out = registry.redact(text);
                if (out.find(stable) != std::string::npos ||
                    out.find("sk-ant-") != std::string::npos) {
                    ++failures;
                }
            }
        });
    }
    for (std::thread& reader : readers) {
        reader.join();
    }
    stop = true;
    writer.join();
    CHECK(failures.load() == 0);
}

// --- redactFinal and redactRegistered
// ---------------------------------------------------------------------

TEST_CASE("redactFinal: a key cut in two by the end of the text loses its start") {
    const std::string key = "GATEWAY-key-0123456789-abcdef";
    Fixture fx({key});
    // The whole key is replaced as redact() does.
    CHECK(fx.registry.redactFinal("The key is " + key + " ok") == "The key is " + kMark + " ok");
    // Every cut from 6 bytes up is removed; the text before it stays.
    for (std::size_t cut = SecretRegistry::kMinPartialSecretBytes; cut < key.size(); ++cut) {
        CAPTURE(cut);
        const std::string out = fx.registry.redactFinal("The key is " + key.substr(0, cut));
        CHECK(out == "The key is ");
    }
    // Shorter starts are left (they would match innocent text), so at most 5 bytes can survive.
    CHECK(fx.registry.redactFinal("The key is GATEW") == "The key is GATEW");
    // A text that merely ends with an ordinary word is untouched.
    CHECK(fx.registry.redactFinal("Check your heir.") == "Check your heir.");
    CHECK(fx.registry.redactFinal("") == "");
}

TEST_CASE("redactFinal: an unfinished key-shaped run is cut, a finished one is replaced") {
    Fixture fx;
    // "sk-ant-" with fewer than 16 key characters after it is not covered by redact().
    CHECK(fx.registry.redact("see sk-ant-FAKE0123") == "see sk-ant-FAKE0123");
    CHECK(fx.registry.redactFinal("see sk-ant-FAKE0123") == "see ");
    CHECK(fx.registry.redactFinal("see sk-ant-") == "see ");
    CHECK(fx.registry.redactFinal("see sk-ant-abc def") == "see sk-ant-abc def");
    CHECK(fx.registry.redactFinal("see " + fakeKey() + " and sk-ant-FAKE01") ==
          "see " + kMark + " and ");
    CHECK(fx.registry.redactFinal("see " + fakeKey()) == "see " + kMark);
}

TEST_CASE("redactFinal: several secrets, and a secret that starts with another's end") {
    Fixture fx({"first-secret-0001", "second-secret-0002"});
    CHECK(fx.registry.redactFinal("a second-sec") == "a ");
    CHECK(fx.registry.redactFinal("a first-secret-00") == "a ");
    CHECK(fx.registry.redactFinal("a second-sec and first-sec") == "a second-sec and ");
}

TEST_CASE("redactFinal: no registry entries and long texts") {
    SecretRegistry empty;
    CHECK(empty.redactFinal("plain text") == "plain text");
    Fixture fx({"long-secret-value-01"});
    const std::string big(200000, 'x');
    CHECK(fx.registry.redactFinal(big + " long-secret-va") == big + " ");
}

TEST_CASE("redactRegistered: only registered secrets, never a shape") {
    const std::string key = "tok-0123456789-abcdef";
    Fixture fx({key});
    CHECK(fx.registry.redactRegistered("a " + key + " b") == "a " + kMark + " b");
    // The shape of a key is not touched: the front of a key that goes on in the next piece.
    CHECK(fx.registry.redactRegistered(fakeKey()) == fakeKey());
    CHECK(fx.registry.redactRegistered("Authorization: Bearer abcdef") ==
          "Authorization: Bearer abcdef");
    CHECK(fx.registry.redactRegistered("") == "");
    SecretRegistry empty;
    CHECK(empty.redactRegistered("anything") == "anything");
    // Two copies, adjacent and apart.
    CHECK(fx.registry.redactRegistered(key + key + " x " + key) == kMark + " x " + kMark);
}
