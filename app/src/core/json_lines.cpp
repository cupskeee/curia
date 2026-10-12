// SPDX-License-Identifier: MIT
#include "core/json_lines.h"

#include <utility>

namespace curia {

namespace {

std::string scrub(const std::string& text, const SecretRegistry* registry) {
    return registry != nullptr ? registry->redact(text) : redact(text);
}

}  // namespace

void redactJsonStrings(nlohmann::json& value, const SecretRegistry* registry) {
    if (value.is_string()) {
        value = scrub(value.get_ref<const std::string&>(), registry);
    } else if (value.is_array() || value.is_object()) {
        for (auto& item : value) {
            redactJsonStrings(item, registry);
        }
    }
}

JsonLineWriter::JsonLineWriter(std::ostream& out, const SecretRegistry* registry)
    : out_(out), registry_(registry) {}

std::string JsonLineWriter::format(nlohmann::json event, const SecretRegistry* registry) {
    redactJsonStrings(event, registry);
    return event.dump(-1, ' ', true, nlohmann::json::error_handler_t::replace);
}

void JsonLineWriter::write(nlohmann::json event) {
    std::string line = format(std::move(event), registry_);
    line.push_back('\n');
    const std::lock_guard<std::mutex> lock(mutex_);
    out_ << line << std::flush;
}

}  // namespace curia
