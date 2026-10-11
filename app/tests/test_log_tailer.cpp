// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include "bridge/log_tailer.h"
#include "bridge_test_support.h"

using namespace curia::bridge;
using namespace curia::bridge::test;

namespace {

struct Rig {
    std::shared_ptr<FakeFile> file = std::make_shared<FakeFile>();
    std::unique_ptr<LogTailer> tailer;

    explicit Rig(bool fromStart = false, const std::string& initial = {}) {
        file->append(initial);
        LogTailer::Options options;
        options.fromStart = fromStart;
        tailer = std::make_unique<LogTailer>(std::make_unique<FakeFileSource>(file), options);
    }
};

std::string exampleFrameText(std::int32_t snap) {
    return stream(exampleFrame(snap));
}

bool hasNotice(const PollResult& result, TailerNotice notice) {
    return std::find(result.notices.begin(), result.notices.end(), notice) != result.notices.end();
}

std::size_t countNotice(const PollResult& result, TailerNotice notice) {
    return static_cast<std::size_t>(
        std::count(result.notices.begin(), result.notices.end(), notice));
}

}  // namespace

TEST_CASE("tailer: attaches at the end of an existing file and does not replay history") {
    Rig rig(false, exampleFrameText(1));
    const PollResult first = rig.tailer->poll();
    CHECK(first.events.empty());
    CHECK(hasNotice(first, TailerNotice::Attached));
    CHECK(rig.tailer->stats().offset == rig.file->size());
    CHECK(rig.tailer->stats().totalBytes == 0);

    rig.file->append(exampleFrameText(2));
    const PollResult second = rig.tailer->poll();
    CHECK(second.notices.empty());
    REQUIRE(second.events.size() == 1);
    const auto* complete = std::get_if<FrameComplete>(&second.events.front());
    REQUIRE(complete != nullptr);
    CHECK(complete->frame.snap == 2);
}

TEST_CASE("tailer: fromStart replays the existing content") {
    Rig rig(true, exampleFrameText(1) + exampleFrameText(2));
    const PollResult result = rig.tailer->poll();
    CHECK(eventsOfType<FrameComplete>(result.events).size() == 2);
    CHECK(rig.tailer->stats().totalBytes == rig.file->size());
}

TEST_CASE("tailer: an empty existing file attaches at 0") {
    Rig rig;
    CHECK(hasNotice(rig.tailer->poll(), TailerNotice::Attached));
    rig.file->append(exampleFrameText(3));
    CHECK(rig.tailer->poll().events.size() == 1);
}

TEST_CASE("tailer: a file that is missing at start and appears later is read from its first byte") {
    Rig rig;
    rig.file->remove();
    const PollResult first = rig.tailer->poll();
    CHECK(countNotice(first, TailerNotice::Missing) == 1);
    CHECK(first.events.empty());
    // Reported once per absence, not once per poll.
    CHECK(rig.tailer->poll().notices.empty());
    CHECK(rig.tailer->poll().notices.empty());

    rig.file->create();
    rig.file->append(exampleFrameText(4));
    const PollResult appeared = rig.tailer->poll();
    CHECK(hasNotice(appeared, TailerNotice::Attached));
    CHECK_FALSE(hasNotice(appeared, TailerNotice::Missing));
    REQUIRE(eventsOfType<FrameComplete>(appeared.events).size() == 1);
}

TEST_CASE("tailer: reads are bounded to 64 KiB and the whole file is read") {
    std::string noise;
    const std::string line = prefixedLine("not a record, only text to fill the log");
    while (noise.size() < 200 * 1024) {
        noise += line;
    }
    Rig rig(true, noise + exampleFrameText(5));
    const PollResult result = rig.tailer->poll();
    CHECK(rig.file->largestRead() == LogTailer::kChunkBytes);
    CHECK(rig.file->readCalls() >= 4);
    CHECK(rig.tailer->stats().totalBytes == rig.file->size());
    CHECK(eventsOfType<FrameComplete>(result.events).size() == 1);
}

TEST_CASE("tailer: a line is held back until its newline arrives") {
    Rig rig;
    rig.tailer->poll();
    const std::string text = exampleFrameText(6);
    const std::size_t cut = text.size() - 3;  // inside the END line
    rig.file->append(text.substr(0, cut));
    CHECK(rig.tailer->poll().events.empty());
    rig.file->append(text.substr(cut));
    CHECK(eventsOfType<FrameComplete>(rig.tailer->poll().events).size() == 1);
}

TEST_CASE("tailer: truncation abandons an open frame and restarts at 0") {
    Rig rig;
    rig.tailer->poll();
    std::vector<std::string> frame = exampleFrame(7);
    frame.pop_back();  // no END yet
    rig.file->append(stream(frame));
    CHECK(rig.tailer->poll().events.empty());
    CHECK(rig.tailer->stats().totalBytes == rig.file->size());

    rig.file->truncate();
    const PollResult truncated = rig.tailer->poll();
    CHECK(countNotice(truncated, TailerNotice::Truncated) == 1);
    const auto damaged = eventsOfType<FrameDamaged>(truncated.events);
    REQUIRE(damaged.size() == 1);
    CHECK(damaged.front()->reason == DamageReason::TruncatedLog);
    CHECK(damaged.front()->snap == 7);
    const TailerStats stats = rig.tailer->stats();
    CHECK(stats.offset == 0);
    CHECK(stats.totalBytes == 0);
    CHECK(stats.curiaBytes == 0);
    CHECK(stats.truncations == 1);

    rig.file->append(exampleFrameText(1));
    const PollResult again = rig.tailer->poll();
    REQUIRE(eventsOfType<FrameComplete>(again.events).size() == 1);
    CHECK(rig.tailer->stats().totalBytes == rig.file->size());
}

TEST_CASE("tailer: a truncation drops an unterminated tail") {
    Rig rig;
    rig.tailer->poll();
    const std::vector<std::string> frame = exampleFrame(8);
    // The BEGIN line without its newline stays in the splitter.
    std::string begin = prefixed(frame.front());
    rig.file->append(begin);
    CHECK(rig.tailer->poll().events.empty());
    rig.file->truncate();
    CHECK(hasNotice(rig.tailer->poll(), TailerNotice::Truncated));
    // If the tail had survived, this newline would complete the BEGIN and the frame below would
    // be complete; with the tail dropped, nothing opens the frame.
    std::string rest = "\n";
    for (std::size_t i = 1; i < frame.size(); ++i) {
        rest += prefixedLine(frame[i], i);
    }
    rig.file->append(rest);
    const PollResult result = rig.tailer->poll();
    CHECK(eventsOfType<FrameComplete>(result.events).empty());
}

TEST_CASE("tailer: a file deleted while attached is a truncation, then it is waited for") {
    Rig rig;
    rig.tailer->poll();
    std::vector<std::string> frame = exampleFrame(9);
    frame.pop_back();
    rig.file->append(stream(frame));
    rig.tailer->poll();

    rig.file->remove();
    const PollResult gone = rig.tailer->poll();
    CHECK(countNotice(gone, TailerNotice::Missing) == 1);
    const auto damaged = eventsOfType<FrameDamaged>(gone.events);
    REQUIRE(damaged.size() == 1);
    CHECK(damaged.front()->reason == DamageReason::TruncatedLog);

    rig.file->create();
    rig.file->append(exampleFrameText(1));
    const PollResult back = rig.tailer->poll();
    CHECK(hasNotice(back, TailerNotice::Attached));
    CHECK(eventsOfType<FrameComplete>(back.events).size() == 1);
}

TEST_CASE("tailer: an unreadable file is reported once and polling continues") {
    Rig rig;
    rig.tailer->poll();
    rig.file->append(exampleFrameText(10));
    rig.file->setReadError(true);
    const PollResult first = rig.tailer->poll();
    CHECK(countNotice(first, TailerNotice::Unreadable) == 1);
    CHECK(first.events.empty());
    CHECK(rig.tailer->poll().notices.empty());
    CHECK(rig.tailer->poll().notices.empty());
    CHECK(rig.tailer->stats().offset == 0);  // nothing was lost

    rig.file->setReadError(false);
    const PollResult recovered = rig.tailer->poll();
    CHECK(hasNotice(recovered, TailerNotice::Recovered));
    CHECK(eventsOfType<FrameComplete>(recovered.events).size() == 1);
}

TEST_CASE("tailer: a file that cannot be examined is reported once") {
    Rig rig;
    rig.tailer->poll();
    rig.file->setStatError(true);
    CHECK(countNotice(rig.tailer->poll(), TailerNotice::Unreadable) == 1);
    CHECK(rig.tailer->poll().notices.empty());
    rig.file->setStatError(false);
    rig.file->append(exampleFrameText(11));
    const PollResult recovered = rig.tailer->poll();
    CHECK(hasNotice(recovered, TailerNotice::Recovered));
    CHECK(eventsOfType<FrameComplete>(recovered.events).size() == 1);
}

TEST_CASE("tailer: counts total bytes and bytes of Curia lines, both reset by a truncation") {
    Rig rig;
    rig.tailer->poll();
    const std::string noiseLine = prefixedLine("hand written noise without the marker");
    const std::string frame = exampleFrameText(12);
    rig.file->append(noiseLine + frame + noiseLine);
    rig.tailer->poll();
    const TailerStats stats = rig.tailer->stats();
    CHECK(stats.totalBytes == rig.file->size());
    CHECK(stats.curiaBytes == frame.size());  // every line of the frame is a Curia line

    rig.file->truncate();
    rig.tailer->poll();
    CHECK(rig.tailer->stats().totalBytes == 0);
    CHECK(rig.tailer->stats().curiaBytes == 0);
}

TEST_CASE("tailer: the events do not depend on how the file grows (all fixtures)") {
    for (const std::string& name : fixtureNames()) {
        CAPTURE(name);
        const Fixture fixture = loadFixture(name);
        REQUIRE(fixture.errors.empty());
        const std::string expected = eventsText(runActions(fixture.actions, 0));

        for (const std::size_t step : {std::size_t{1}, std::size_t{97}, std::size_t{100000}}) {
            CAPTURE(step);
            Rig rig;
            std::vector<Event> events;
            const auto take = [&events](PollResult&& result) {
                for (Event& e : result.events) {
                    events.push_back(std::move(e));
                }
            };
            take(rig.tailer->poll());
            for (const Action& action : fixture.actions) {
                if (action.kind == Action::Kind::Truncate) {
                    rig.file->truncate();
                    take(rig.tailer->poll());
                    continue;
                }
                for (std::size_t pos = 0; pos < action.bytes.size(); pos += step) {
                    rig.file->append(std::string_view(action.bytes).substr(pos, step));
                    take(rig.tailer->poll());
                }
            }
            CHECK(eventsText(events) == expected);
        }
    }
}

TEST_CASE("file source: stat and read on a real file, and the tailer on top of it") {
    const TempDir dir;
    const std::string path = dir.path("debug.log");

    FileSource source(path);
    CHECK(source.stat().state == IFileSource::State::Missing);
    std::string sink;
    CHECK_FALSE(source.read(0, 10, sink));

    {
        std::ofstream out(path, std::ios::binary);
        out << "hello\nworld\n";
    }
    const IFileSource::Stat stat = source.stat();
    CHECK(stat.state == IFileSource::State::Ok);
    CHECK(stat.size == 12);
    std::string got;
    CHECK(source.read(6, 100, got));
    CHECK(got == "world\n");
    got.clear();
    CHECK(source.read(12, 10, got));  // at the end: no bytes, not an error
    CHECK(got.empty());

    // Through a tailer: the file is appended to and truncated in place, as the game does.
    LogTailer tailer(std::make_unique<FileSource>(path));
    CHECK(tailer.poll().events.empty());  // attached at the end
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << exampleFrameText(20);
    }
    const PollResult appended = tailer.poll();
    REQUIRE(eventsOfType<FrameComplete>(appended.events).size() == 1);

    { std::ofstream out(path, std::ios::binary | std::ios::trunc); }
    CHECK(hasNotice(tailer.poll(), TailerNotice::Truncated));
    {
        std::ofstream out(path, std::ios::binary | std::ios::app);
        out << exampleFrameText(21);
    }
    CHECK(eventsOfType<FrameComplete>(tailer.poll().events).size() == 1);
}
