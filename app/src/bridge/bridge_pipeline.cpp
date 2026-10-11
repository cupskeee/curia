// SPDX-License-Identifier: MIT
#include "bridge/bridge_pipeline.h"

#include <utility>
#include <variant>

#include "bridge/record.h"

namespace curia::bridge {

std::vector<Event> BridgePipeline::feed(std::string_view bytes) {
    std::vector<Event> events;
    splitter_.feed(bytes, [this, &events](Line&& line) { feedLineInto(line, events); });
    return events;
}

std::vector<Event> BridgePipeline::truncateLog() {
    splitter_.reset();
    std::vector<Event> events;
    collect(assembler_.truncateLog(), events);
    return events;
}

std::vector<Event> BridgePipeline::feedLine(const Line& line) {
    std::vector<Event> events;
    feedLineInto(line, events);
    return events;
}

void BridgePipeline::feedLineInto(const Line& line, std::vector<Event>& events) {
    collect(assembler_.consume(parseLine(line.text, line.overlong)), events);
}

void BridgePipeline::collect(std::vector<Event>&& from, std::vector<Event>& to) {
    for (Event& event : from) {
        if (std::holds_alternative<Diagnostic>(event)) {
            if (diagnosticRun_ >= kMaxDiagnosticsInARun) {
                ++suppressedDiagnostics_;
                continue;
            }
            ++diagnosticRun_;
        } else {
            diagnosticRun_ = 0;
        }
        to.push_back(std::move(event));
    }
}

}  // namespace curia::bridge
