// SPDX-License-Identifier: MIT
#include "core/headless_commands.h"

#include <cctype>
#include <utility>

namespace curia {

namespace {

constexpr std::string_view kBlanks = " \t\r\n";

std::string_view trim(std::string_view text) {
    const std::size_t first = text.find_first_not_of(kBlanks);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kBlanks);
    return text.substr(first, last - first + 1);
}

std::string lower(std::string_view text) {
    std::string out(text);
    for (char& ch : out) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return out;
}

HeadlessCommand invalid(std::string problem) {
    HeadlessCommand command;
    command.kind = HeadlessCommand::Kind::Invalid;
    command.problem = std::move(problem);
    return command;
}

}  // namespace

HeadlessCommand parseHeadlessCommand(std::string_view line) {
    if (line.size() > kMaxCommandLineBytes) {
        return invalid("the line is too long");
    }
    const std::string_view trimmed = trim(line);
    if (trimmed.empty()) {
        return {};
    }
    const std::size_t verbEnd = trimmed.find_first_of(" \t");
    const std::string verb = lower(trimmed.substr(0, verbEnd));
    const std::string_view rest =
        verbEnd == std::string_view::npos ? std::string_view{} : trim(trimmed.substr(verbEnd));

    HeadlessCommand command;
    if (verb == "ask") {
        if (rest.empty()) {
            return invalid("ask needs a question");
        }
        command.kind = HeadlessCommand::Kind::Ask;
        command.text = std::string(rest);
        return command;
    }
    const bool bare = rest.empty();
    if (verb == "cancel" || verb == "arm" || verb == "refresh" || verb == "retry" ||
        verb == "quit") {
        if (!bare) {
            return invalid(verb + " takes no argument");
        }
        if (verb == "cancel") {
            command.kind = HeadlessCommand::Kind::Cancel;
        } else if (verb == "arm") {
            command.kind = HeadlessCommand::Kind::Arm;
        } else if (verb == "refresh") {
            command.kind = HeadlessCommand::Kind::Refresh;
        } else if (verb == "retry") {
            command.kind = HeadlessCommand::Kind::Retry;
        } else {
            command.kind = HeadlessCommand::Kind::Quit;
        }
        return command;
    }
    if (verb == "ck3") {
        const std::string state = lower(rest);
        if (state == "running") {
            command.kind = HeadlessCommand::Kind::Ck3;
            command.ck3Running = true;
            return command;
        }
        if (state == "stopped") {
            command.kind = HeadlessCommand::Kind::Ck3;
            command.ck3Running = false;
            return command;
        }
        return invalid("ck3 needs running or stopped");
    }
    return invalid("unknown command");
}

}  // namespace curia
