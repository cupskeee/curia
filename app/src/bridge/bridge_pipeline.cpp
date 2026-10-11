// SPDX-License-Identifier: MIT
#include "bridge/bridge_pipeline.h"

#include <iterator>

#include "bridge/record.h"

namespace curia::bridge {

namespace {

void append(std::vector<Event>& to, std::vector<Event>&& from) {
    to.insert(to.end(), std::make_move_iterator(from.begin()), std::make_move_iterator(from.end()));
}

}  // namespace

std::vector<Event> BridgePipeline::feed(std::string_view bytes) {
    std::vector<Event> events;
    for (const Line& line : splitter_.feed(bytes)) {
        append(events, feedLine(line));
    }
    return events;
}

std::vector<Event> BridgePipeline::truncateLog() {
    splitter_.reset();
    return assembler_.truncateLog();
}

std::vector<Event> BridgePipeline::feedLine(const Line& line) {
    return assembler_.consume(parseLine(line.text, line.overlong));
}

}  // namespace curia::bridge
