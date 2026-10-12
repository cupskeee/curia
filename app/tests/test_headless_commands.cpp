// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>

#include "core/headless_commands.h"

using curia::HeadlessCommand;
using curia::parseHeadlessCommand;
using Kind = curia::HeadlessCommand::Kind;

TEST_CASE("headless commands: ask takes the rest of the line") {
    const HeadlessCommand command = parseHeadlessCommand("ask  what should I focus on?  ");
    CHECK((command.kind == Kind::Ask));
    CHECK(command.text == "what should I focus on?");
    CHECK((parseHeadlessCommand("ASK Hello").kind == Kind::Ask));
    CHECK(parseHeadlessCommand("Ask a   b").text == "a   b");
    CHECK(parseHeadlessCommand("ask\tTabbed question\r").text == "Tabbed question");
}

TEST_CASE("headless commands: ask without a question is invalid") {
    CHECK((parseHeadlessCommand("ask").kind == Kind::Invalid));
    CHECK((parseHeadlessCommand("ask   ").kind == Kind::Invalid));
}

TEST_CASE("headless commands: the bare verbs") {
    CHECK((parseHeadlessCommand("cancel").kind == Kind::Cancel));
    CHECK((parseHeadlessCommand("  arm\r").kind == Kind::Arm));
    CHECK((parseHeadlessCommand("refresh").kind == Kind::Refresh));
    CHECK((parseHeadlessCommand("retry").kind == Kind::Retry));
    CHECK((parseHeadlessCommand("QUIT").kind == Kind::Quit));
    CHECK((parseHeadlessCommand("cancel now").kind == Kind::Invalid));
    CHECK((parseHeadlessCommand("quit please").kind == Kind::Invalid));
}

TEST_CASE("headless commands: ck3 running or stopped") {
    HeadlessCommand command = parseHeadlessCommand("ck3 running");
    CHECK((command.kind == Kind::Ck3));
    CHECK(command.ck3Running);
    command = parseHeadlessCommand("CK3   Stopped");
    CHECK((command.kind == Kind::Ck3));
    CHECK_FALSE(command.ck3Running);
    CHECK((parseHeadlessCommand("ck3").kind == Kind::Invalid));
    CHECK((parseHeadlessCommand("ck3 maybe").kind == Kind::Invalid));
}

TEST_CASE("headless commands: empty lines, unknown verbs and long lines") {
    CHECK((parseHeadlessCommand("").kind == Kind::Nothing));
    CHECK((parseHeadlessCommand("  \t\r").kind == Kind::Nothing));
    CHECK((parseHeadlessCommand("dance").kind == Kind::Invalid));
    const std::string secret = std::string("sk-ant-") + "FAKE0123456789abcdefghij";
    const HeadlessCommand invalid = parseHeadlessCommand("bogus " + secret);
    CHECK((invalid.kind == Kind::Invalid));
    CHECK(invalid.problem.find("FAKE") == std::string::npos);  // the line is never repeated
    const std::string longLine(curia::kMaxCommandLineBytes + 1, 'a');
    CHECK((parseHeadlessCommand("ask " + longLine).kind == Kind::Invalid));
    CHECK((parseHeadlessCommand("ask " + std::string(1000, 'a')).kind == Kind::Ask));
}
