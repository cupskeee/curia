// SPDX-License-Identifier: MIT
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "bridge/frame.h"
#include "core/clock.h"
#include "providers/provider.h"

// Helpers shared by the advisor tests: a clock that only moves when told, a scripted provider,
// synthetic frames (invented names, built through the real parser and assembler), golden files.
namespace curia::advisor::test {

// The value of an optional that the test has already shown to be there (non-const form of
// bridge::test::must), so that static analysis sees the check.
template <typename T> T& mustRef(std::optional<T>& value) {
    if (!value.has_value()) {
        throw std::runtime_error("expected a value");
    }
    return *value;
}

struct FakeClock {
    TimePoint now = TimePoint{} + std::chrono::hours(1);

    ClockFn fn() {
        return [this] { return now; };
    }
    void advance(std::chrono::seconds seconds) { now += seconds; }
};

// The records of a frame without the "CURIA1|<snap>|" prefix, for example "FIN|312|78"; this adds
// the prefix and the END record. The first record must be a BEGIN.
bridge::Frame makeFrame(const std::vector<std::string>& records, int snap = 7);

// A realistic frame: every section, 14 vassals of which 10 are listed, two wars, three claims, one
// faction against the ruler and the ruler's own.
bridge::Frame richFrame();
// BEGIN and END only: no section exported.
bridge::Frame minimalFrame();
// Every section present but empty, a debt and a negative income.
bridge::Frame emptyFrame();
// Game strings made to look like instructions and markers (the block's own end marker among
// them, plain and in lookalike forms).
bridge::Frame hostileFrame();
// Vassals total/listed for the boundary tests (listed vassals are named "Test Vassal <rank>").
bridge::Frame frameWithVassals(int total, int listed);
// A frame with the given date and gold, otherwise minimal plus a heir (for the end-to-end tests).
bridge::Frame frameWithGoldAndHeir(int year, int month, int day, int gold,
                                   const std::string& heirName);

// Compares text with tests/golden/<name>.txt byte for byte (the file ends in one newline).
// CURIA_UPDATE_GOLDEN=1 rewrites the file; review the diff by hand.
void checkGoldenText(const std::string& name, const std::string& text);

// A provider that records every call and either waits for the test to drive its handlers
// (manual, the default) or plays a script (inline or on a thread of its own).
class FakeProvider final : public providers::Provider {
public:
    struct Call {
        providers::ChatRequest request;
        providers::StreamHandlers handlers;
        providers::CancelToken cancel;
    };

    struct Script {
        std::vector<std::string> tokens;
        providers::Usage usage;
        // Instead of onDone.
        std::optional<providers::ChatError> error;
        // Play on a new thread (otherwise inline, inside streamChat).
        bool onThread = false;
        // Pause between tokens when on a thread.
        std::chrono::milliseconds gap{0};
        // Report a thinking block (ThinkingStarted, then TextStarted) before the first token.
        bool thinkingFirst = false;
    };

    FakeProvider() = default;
    ~FakeProvider() override;

    void streamChat(const providers::ChatRequest& request, providers::StreamHandlers handlers,
                    providers::CancelToken cancel) override;

    void setScript(Script script);
    void clearScript();

    std::size_t callCount() const;
    Call call(std::size_t index) const;
    Call lastCall() const;

private:
    static void play(const Script& script, const Call& call);

    mutable std::mutex mutex_;
    std::vector<Call> calls_;
    std::optional<Script> script_;
    std::vector<std::thread> threads_;
};

// Waits (polling with short sleeps, bounded) until `done()` is true; returns whether it was.
bool waitUntil(const std::function<bool()>& done,
               std::chrono::milliseconds limit = std::chrono::seconds(10));

}  // namespace curia::advisor::test
