// SPDX-License-Identifier: MIT
#include "core/redact.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace curia {

namespace detail {

// Aho-Corasick over the registered secrets. Nodes keep their children in a small vector: a node
// has at most kMaxSecrets children, so a lookup is a handful of byte compares, and the automaton
// stays small (at most kMaxSecrets * kMaxSecretBytes + 1 nodes) without a 256-wide table per node.
class SecretMatcher {
public:
    explicit SecretMatcher(const std::vector<std::string>& secrets) {
        nodes_.emplace_back();
        for (const std::string& secret : secrets) {
            std::uint32_t at = 0;
            for (const char ch : secret) {
                const auto byte = static_cast<unsigned char>(ch);
                std::uint32_t next = child(at, byte);
                if (next == 0) {
                    next = static_cast<std::uint32_t>(nodes_.size());
                    nodes_.emplace_back();
                    nodes_[at].children.emplace_back(byte, next);
                }
                at = next;
            }
            nodes_[at].longest = static_cast<std::uint32_t>(secret.size());
        }
        // Failure links by breadth-first order, so a node's link is known before its children's.
        std::vector<std::uint32_t> queue;
        for (const auto& edge : nodes_[0].children) {
            queue.push_back(edge.second);
        }
        for (std::size_t head = 0; head < queue.size(); ++head) {
            const std::uint32_t at = queue[head];
            for (const auto& edge : nodes_[at].children) {
                const unsigned char byte = edge.first;
                const std::uint32_t node = edge.second;
                std::uint32_t link = nodes_[at].fail;
                while (link != 0 && child(link, byte) == 0) {
                    link = nodes_[link].fail;
                }
                const std::uint32_t target = child(link, byte);
                nodes_[node].fail = target != node ? target : 0;
                nodes_[node].longest =
                    std::max(nodes_[node].longest, nodes_[nodes_[node].fail].longest);
                queue.push_back(node);
            }
        }
    }

    ~SecretMatcher() {
        // The trie spells the secrets out; clear the edge bytes before the memory is returned.
        for (Node& node : nodes_) {
            secureZero(node.children.data(), node.children.size() * sizeof(Edge));
        }
    }

    SecretMatcher(const SecretMatcher&) = delete;
    SecretMatcher& operator=(const SecretMatcher&) = delete;

    // Calls sink(start, end) for the longest secret that ends at each position of the text, in
    // increasing order of end. Each byte costs one amortised-constant step: following a failure
    // link never goes deeper than the depth reached, and the depth grows by at most one per byte.
    template <typename Sink> void scan(std::string_view text, Sink&& sink) const {
        std::uint32_t state = 0;
        for (std::size_t i = 0; i < text.size(); ++i) {
            const auto byte = static_cast<unsigned char>(text[i]);
            for (;;) {
                const std::uint32_t next = child(state, byte);
                if (next != 0) {
                    state = next;
                    break;
                }
                if (state == 0) {
                    break;
                }
                state = nodes_[state].fail;
            }
            const std::uint32_t longest = nodes_[state].longest;
            if (longest != 0) {
                sink(i + 1 - longest, i + 1);
            }
        }
    }

private:
    using Edge = std::pair<unsigned char, std::uint32_t>;

    struct Node {
        std::vector<Edge> children;
        std::uint32_t fail = 0;
        // Length of the longest secret that is a suffix of the text spelled by this node.
        std::uint32_t longest = 0;
    };

    // 0 means "none": the root is never a child.
    [[nodiscard]] std::uint32_t child(std::uint32_t at, unsigned char byte) const {
        for (const Edge& edge : nodes_[at].children) {
            if (edge.first == byte) {
                return edge.second;
            }
        }
        return 0;
    }

    std::vector<Node> nodes_;
};

}  // namespace detail

namespace {

// A half-open byte range [start, end).
struct Span {
    std::size_t start;
    std::size_t end;
};

// A set of byte ranges kept as sorted, disjoint, non-touching spans. add() must be called with
// non-decreasing `end`: then every stored span that overlaps or touches the new one is a suffix of
// the stack, so merging pops them and each span is popped at most once (amortised O(1)).
class SpanList {
public:
    void add(std::size_t start, std::size_t end) {
        while (!spans_.empty() && spans_.back().end >= start) {
            start = std::min(start, spans_.back().start);
            spans_.pop_back();
        }
        spans_.push_back({start, end});
    }
    [[nodiscard]] const std::vector<Span>& spans() const { return spans_; }
    [[nodiscard]] bool empty() const { return spans_.empty(); }

private:
    std::vector<Span> spans_;
};

constexpr std::string_view kKeyPrefix = "sk-ant-";
constexpr std::size_t kKeyMinTail = 16;
constexpr std::string_view kBearer = "bearer";
constexpr std::string_view kHeaderNames[] = {"authorization", "x-api-key", "proxy-authorization"};

char lowerAscii(char ch) {
    return (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
}

bool matchesNoCase(std::string_view text, std::size_t at, std::string_view lowerWord) {
    if (text.size() - at < lowerWord.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lowerWord.size(); ++i) {
        if (lowerAscii(text[at + i]) != lowerWord[i]) {
            return false;
        }
    }
    return true;
}

bool isBlank(char ch) {
    return ch == ' ' || ch == '\t';
}

bool isKeyChar(char ch) {
    return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
           ch == '_' || ch == '-';
}

// A byte that can belong to a bearer token: not blank or control, not a quote.
bool isTokenChar(char ch) {
    const auto byte = static_cast<unsigned char>(ch);
    return byte > 0x20 && byte != 0x7f && ch != '"' && ch != '\'';
}

std::size_t skipBlanks(std::string_view text, std::size_t at) {
    while (at < text.size() && isBlank(text[at])) {
        ++at;
    }
    return at;
}

// The three pattern families of redact(), found in one left-to-right pass. Each family collects
// its own spans in order of start with non-decreasing end (see SpanList), and caches the end of
// the line or run it just measured: several keywords on one line, or "sk-ant-" repeated inside one
// long run, would otherwise measure the same bytes again and again.
class PatternScanner {
public:
    explicit PatternScanner(std::string_view text) : text_(text) {}

    void run() {
        for (std::size_t i = 0; i < text_.size(); ++i) {
            const char ch = text_[i];
            if (ch == 's' && text_.compare(i, kKeyPrefix.size(), kKeyPrefix) == 0) {
                keyAt(i);
            }
            switch (lowerAscii(ch)) {
            case 'a':
                headerAt(i, kHeaderNames[0]);
                break;
            case 'x':
                headerAt(i, kHeaderNames[1]);
                break;
            case 'p':
                headerAt(i, kHeaderNames[2]);
                break;
            case 'b':
                bearerAt(i);
                break;
            default:
                break;
            }
        }
    }

    [[nodiscard]] const SpanList& keys() const { return keys_; }
    [[nodiscard]] const SpanList& headers() const { return headers_; }
    [[nodiscard]] const SpanList& bearers() const { return bearers_; }

private:
    void keyAt(std::size_t at) {
        const std::size_t tail = at + kKeyPrefix.size();
        if (tail < runStart_ || tail > runEnd_) {
            runStart_ = tail;
            runEnd_ = tail;
            while (runEnd_ < text_.size() && isKeyChar(text_[runEnd_])) {
                ++runEnd_;
            }
        }
        if (runEnd_ - tail >= kKeyMinTail) {
            keys_.add(at, runEnd_);
        }
    }

    void headerAt(std::size_t at, std::string_view name) {
        if (!matchesNoCase(text_, at, name)) {
            return;
        }
        std::size_t pos = skipBlanks(text_, at + name.size());
        if (pos < text_.size() && (text_[pos] == '"' || text_[pos] == '\'')) {
            pos = skipBlanks(text_, pos + 1);
        }
        if (pos >= text_.size() || text_[pos] != ':') {
            return;
        }
        const std::size_t value = skipBlanks(text_, pos + 1);
        const std::size_t end = lineEnd(value);
        if (value < end) {
            headers_.add(value, end);
        }
    }

    void bearerAt(std::size_t at) {
        if (!matchesNoCase(text_, at, kBearer)) {
            return;
        }
        const std::size_t afterWord = at + kBearer.size();
        const std::size_t start = skipBlanks(text_, afterWord);
        if (start == afterWord) {
            return;  // "Bearer" must be followed by a blank
        }
        std::size_t end = start;
        while (end < text_.size() && isTokenChar(text_[end])) {
            ++end;
        }
        if (start < end) {
            bearers_.add(start, end);
        }
    }

    // The offset of the CR, LF or end of text that ends the line containing `from`.
    std::size_t lineEnd(std::size_t from) {
        if (from >= lineStart_ && from <= lineEnd_ && lineValid_) {
            return lineEnd_;
        }
        std::size_t end = from;
        while (end < text_.size() && text_[end] != '\n' && text_[end] != '\r') {
            ++end;
        }
        lineStart_ = from;
        lineEnd_ = end;
        lineValid_ = true;
        return end;
    }

    std::string_view text_;
    SpanList keys_;
    SpanList headers_;
    SpanList bearers_;
    std::size_t runStart_ = 1;  // an empty cache: runStart_ > runEnd_
    std::size_t runEnd_ = 0;
    std::size_t lineStart_ = 0;
    std::size_t lineEnd_ = 0;
    bool lineValid_ = false;
};

// One pass of redaction. Returns false if nothing matched (out is untouched). Otherwise out is the
// text with every covered run replaced by one marker, which may equal the input (a header value
// that already is the marker).
bool redactRound(std::string_view text, const detail::SecretMatcher* matcher, std::string& out) {
    SpanList secrets;
    if (matcher != nullptr) {
        matcher->scan(text,
                      [&secrets](std::size_t start, std::size_t end) { secrets.add(start, end); });
    }
    PatternScanner scanner(text);
    scanner.run();

    const SpanList* lists[] = {&secrets, &scanner.keys(), &scanner.headers(), &scanner.bearers()};
    std::size_t cursor[] = {0, 0, 0, 0};
    bool any = false;
    for (const SpanList* list : lists) {
        any = any || !list->empty();
    }
    if (!any) {
        return false;
    }

    // Four-way merge of the sorted lists by start, joining overlapping and touching spans.
    std::string result;
    result.reserve(text.size() + kRedactedMarker.size());
    std::size_t copied = 0;
    bool open = false;
    Span current{0, 0};
    const auto flush = [&] {
        result.append(text.substr(copied, current.start - copied));
        result.append(kRedactedMarker);
        copied = current.end;
    };
    for (;;) {
        int best = -1;
        for (int i = 0; i < 4; ++i) {
            if (cursor[i] < lists[i]->spans().size() &&
                (best < 0 ||
                 lists[i]->spans()[cursor[i]].start < lists[best]->spans()[cursor[best]].start)) {
                best = i;
            }
        }
        if (best < 0) {
            break;
        }
        const Span next = lists[best]->spans()[cursor[best]++];
        if (open && next.start <= current.end) {
            current.end = std::max(current.end, next.end);
        } else {
            if (open) {
                flush();
            }
            current = next;
            open = true;
        }
    }
    if (open) {
        flush();
    }
    result.append(text.substr(copied));
    out = std::move(result);
    return true;
}

}  // namespace

SecretRegistry::SecretRegistry() = default;

SecretRegistry::~SecretRegistry() {
    for (std::string& secret : secrets_) {
        wipe(secret);
    }
}

SecretRegistry& SecretRegistry::instance() {
    static auto* const registry = new SecretRegistry();
    return *registry;
}

bool SecretRegistry::add(std::string_view secret) {
    if (secret.size() < kMinSecretBytes || secret.size() > kMaxSecretBytes ||
        kRedactedMarker.find(secret) != std::string_view::npos) {
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (std::find(secrets_.begin(), secrets_.end(), secret) != secrets_.end()) {
        return true;
    }
    if (secrets_.size() >= kMaxSecrets) {
        return false;
    }
    secrets_.emplace_back(secret);
    rebuildLocked();
    return true;
}

bool SecretRegistry::add(const SecretString& secret) {
    return add(secret.reveal());
}

bool SecretRegistry::remove(std::string_view secret) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find(secrets_.begin(), secrets_.end(), secret);
    if (found == secrets_.end()) {
        return false;
    }
    wipe(*found);
    secrets_.erase(found);
    rebuildLocked();
    return true;
}

void SecretRegistry::clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::string& secret : secrets_) {
        wipe(secret);
    }
    secrets_.clear();
    rebuildLocked();
}

std::size_t SecretRegistry::size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return secrets_.size();
}

void SecretRegistry::rebuildLocked() {
    matcher_ = secrets_.empty() ? nullptr : std::make_shared<const detail::SecretMatcher>(secrets_);
}

std::string SecretRegistry::redact(std::string_view text) const {
    std::shared_ptr<const detail::SecretMatcher> matcher;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        matcher = matcher_;
    }
    std::string current;  // the text of the latest round; `view` points at it once there is one
    std::string_view view = text;
    for (int round = 0; round < kMaxRounds; ++round) {
        std::string next;
        if (!redactRound(view, matcher.get(), next) || next == view) {
            return std::string(view);
        }
        current = std::move(next);
        view = current;
    }
    // Still changing after kMaxRounds: give up on keeping the text, keep the guarantee.
    return std::string(kRedactedMarker);
}

std::string redact(std::string_view text) {
    return SecretRegistry::instance().redact(text);
}

}  // namespace curia
