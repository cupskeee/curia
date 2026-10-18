// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>
#include <string_view>
#include <vector>

#include "core/cli.h"

namespace {

bool parse(std::vector<const char*> args, curia::CliOptions& out, std::string& error) {
    args.insert(args.begin(), "curia");
    return curia::parseCli(static_cast<int>(args.size()), args.data(), out, error);
}

// As on an OS with (macOS) or without (Windows, Linux) the overlay panel.
bool parseOn(bool panel, std::vector<const char*> args, curia::CliOptions& out,
             std::string& error) {
    args.insert(args.begin(), "curia");
    return curia::parseCli(static_cast<int>(args.size()), args.data(), out, error, panel);
}

}  // namespace

TEST_CASE("no arguments runs the normal app") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({}, options, error));
    CHECK_FALSE(options.smoke);
    CHECK(options.idleSeconds == 0);
}

TEST_CASE("flags are recognised") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--smoke", "--version"}, options, error));
    CHECK(options.smoke);
    CHECK(options.showVersion);
}

TEST_CASE("--idle-test needs a sensible number") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--idle-test", "10"}, options, error));
    CHECK(options.idleSeconds == 10);
    CHECK_FALSE(parse({"--idle-test"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "0"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "abc"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "5x"}, options, error));
}

TEST_CASE("unknown arguments and conflicting modes are rejected") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--nope"}, options, error));
    CHECK_FALSE(error.empty());
    CHECK_FALSE(parse({"--smoke", "--idle-test", "3"}, options, error));
}

TEST_CASE("--watch-log and its companions") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--watch-log", "some.log"}, options, error));
    CHECK(options.watchLogPath == "some.log");
    CHECK_FALSE(options.fromStart);
    CHECK(options.maxFrames == 0);
    CHECK(options.timeoutSeconds == 0);

    CHECK(parse(
        {"--watch-log", "a.log", "--from-start", "--max-frames", "3", "--timeout-seconds", "20"},
        options, error));
    CHECK(options.fromStart);
    CHECK(options.maxFrames == 3);
    CHECK(options.timeoutSeconds == 20);
    // Order does not matter.
    CHECK(parse({"--max-frames", "1", "--watch-log", "a.log"}, options, error));
    CHECK(options.maxFrames == 1);
}

TEST_CASE("--watch-log needs a path and sensible numbers") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--watch-log"}, options, error));
    CHECK_FALSE(parse({"--watch-log", ""}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames", "0"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--max-frames", "x"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "0"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "5s"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--timeout-seconds", "86401"}, options, error));
}

TEST_CASE("--watch-log conflicts with the window modes, and its options need it") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--watch-log", "a.log", "--smoke"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "3", "--watch-log", "a.log"}, options, error));
    CHECK_FALSE(parse({"--from-start"}, options, error));
    CHECK_FALSE(parse({"--max-frames", "2"}, options, error));
    CHECK_FALSE(parse({"--timeout-seconds", "2"}, options, error));
}

TEST_CASE("--headless-chat and its options") {
    curia::CliOptions options;
    std::string error;
    CHECK_FALSE(parse({"--headless-chat"}, options, error));  // needs --log
    CHECK(parse({"--headless-chat", "--log", "debug.log"}, options, error));
    CHECK(options.headlessChat);
    CHECK(options.logPath == "debug.log");
    CHECK_FALSE(options.frameSnap.has_value());
    CHECK(options.auth == curia::AuthMode::Bearer);
    CHECK(parse({"--headless-chat", "--log", "d.log", "--from-start", "--frame", "12",
                 "--max-tokens", "300", "--show-thinking", "--test-endpoint", "http://127.0.0.1:1",
                 "--auth", "x-api-key", "--log-dir", "logs", "--timeout-seconds", "30"},
                options, error));
    CHECK(options.fromStart);
    CHECK(options.frameSnap == 12);
    CHECK(options.maxTokens == 300);
    CHECK(options.showThinking);
    CHECK(options.testEndpoint == "http://127.0.0.1:1");
    CHECK(options.auth == curia::AuthMode::XApiKey);
    CHECK(options.appLogDir == "logs");
    CHECK(options.timeoutSeconds == 30);
    CHECK_FALSE(options.hasTestOnlyOptions());
}

TEST_CASE("--headless-chat: bad values and misplaced options") {
    curia::CliOptions options;
    std::string error;
    const std::vector<const char*> base = {"--headless-chat", "--log", "d.log"};
    auto with = [&](std::vector<const char*> extra) {
        std::vector<const char*> args = base;
        args.insert(args.end(), extra.begin(), extra.end());
        return parse(args, options, error);
    };
    // A snapshot number is only found by reading the log from its start.
    CHECK_FALSE(with({"--frame", "3"}));
    CHECK(error.find("--from-start") != std::string::npos);
    CHECK(with({"--from-start", "--frame", "3"}));
    CHECK_FALSE(with({"--frame", "0"}));
    CHECK_FALSE(with({"--frame", "x"}));
    CHECK_FALSE(with({"--max-tokens", "0"}));
    CHECK_FALSE(with({"--max-tokens", "64001"}));
    CHECK_FALSE(with({"--auth", "basic"}));
    CHECK_FALSE(with({"--auth"}));
    CHECK_FALSE(with({"--log-dir", ""}));
    CHECK_FALSE(with({"--smoke"}));
    CHECK_FALSE(with({"--check-api"}));
    // The options belong to the modes that use them (--log, --test-endpoint, --auth and
    // --log-dir also to the chat window, see below).
    CHECK_FALSE(parse({"--frame", "3"}, options, error));
    CHECK_FALSE(parse({"--smoke", "--log-dir", "x"}, options, error));
    CHECK_FALSE(parse({"--smoke", "--log", "d.log"}, options, error));
    CHECK_FALSE(
        parse({"--idle-test", "3", "--test-endpoint", "http://127.0.0.1:1"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--log", "d.log"}, options, error));
    CHECK_FALSE(parse({"--max-tokens", "300"}, options, error));
}

TEST_CASE("test-only timing options are parsed and flagged") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--headless-chat", "--log", "d.log", "--armed-wait-ms", "700",
                 "--arm-snapshot-age-ms", "0", "--passive-silence-ms", "900", "--tick-ms", "20",
                 "--poll-ms", "30"},
                options, error));
    CHECK(options.armedWaitMs == 700);
    CHECK(options.armSnapshotAgeMs == 0);
    CHECK(options.passiveSilenceMs == 900);
    CHECK(options.tickMs == 20);
    CHECK(options.pollMs == 30);
    CHECK(options.hasTestOnlyOptions());
    CHECK(parse({"--headless-chat", "--log", "d.log", "--selftest-throw"}, options, error));
    CHECK(options.selftestThrow);
    CHECK(options.hasTestOnlyOptions());
    CHECK_FALSE(parse({"--headless-chat", "--log", "d.log", "--tick-ms", "0"}, options, error));
    CHECK_FALSE(parse({"--armed-wait-ms", "5"}, options, error));
    CHECK_FALSE(parse({"--selftest-throw"}, options, error));
}

TEST_CASE("--check-api and --cache-probe") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--check-api"}, options, error));
    CHECK(options.checkApi);
    CHECK_FALSE(options.cacheProbe);
    CHECK(parse({"--check-api", "--cache-probe", "--auth", "x-api-key", "--test-endpoint",
                 "http://127.0.0.1:9", "--log-dir", "d"},
                options, error));
    CHECK(options.cacheProbe);
    CHECK(options.auth == curia::AuthMode::XApiKey);
    CHECK_FALSE(parse({"--cache-probe"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--smoke"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--max-tokens", "5"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--from-start"}, options, error));
}

TEST_CASE("--effort and --thinking, with --headless-chat and with --check-api") {
    using curia::providers::Effort;
    using curia::providers::Thinking;
    curia::CliOptions options;
    std::string error;
    // No flag: Curia's default effort (D22) and no thinking setting.
    CHECK(parse({"--headless-chat", "--log", "d.log"}, options, error));
    CHECK((options.effort == curia::providers::kDefaultEffort));
    CHECK((options.effort == Effort::Low));
    CHECK((options.thinking == Thinking::Default));
    CHECK_FALSE(options.effortGiven);
    CHECK_FALSE(options.thinkingGiven);
    CHECK(parse({"--check-api"}, options, error));
    CHECK((options.effort == Effort::Low));
    CHECK_FALSE(options.effortGiven);
    // --effort default sends none (the model's own default, what rounds 1 and 2 called default).
    CHECK(parse({"--headless-chat", "--log", "d.log", "--effort", "default"}, options, error));
    CHECK((options.effort == Effort::Default));
    CHECK(options.effortGiven);
    CHECK(parse({"--check-api", "--effort", "default", "--thinking", "disabled"}, options, error));
    CHECK((options.effort == Effort::Default));
    CHECK((options.thinking == Thinking::Disabled));
    CHECK(options.thinkingGiven);
    // --effort low is the same value as no flag, but given.
    CHECK(parse({"--check-api", "--effort", "low"}, options, error));
    CHECK((options.effort == Effort::Low));
    CHECK(options.effortGiven);
    CHECK(parse(
        {"--headless-chat", "--log", "d.log", "--effort", "low", "--thinking", "between_tools"},
        options, error));
    CHECK((options.effort == Effort::Low));
    CHECK((options.thinking == Thinking::BetweenTools));
    CHECK_FALSE(options.hasTestOnlyOptions());  // no secret in them: any build takes them
    CHECK(parse({"--check-api", "--effort", "xhigh"}, options, error));
    CHECK((options.effort == Effort::XHigh));
    CHECK(parse({"--check-api", "--cache-probe", "--thinking", "disabled", "--effort", "max"},
                options, error));
    CHECK((options.thinking == Thinking::Disabled));
    CHECK((options.effort == Effort::Max));
    for (const char* word : {"medium", "high"}) {
        CHECK(parse({"--check-api", "--effort", word}, options, error));
    }
    CHECK(parse({"--check-api", "--thinking", "adaptive"}, options, error));
    CHECK((options.thinking == Thinking::Adaptive));

    // Bad or missing values are usage errors that name the choices.
    CHECK_FALSE(parse({"--check-api", "--effort", "extreme"}, options, error));
    CHECK(error.find("default, low, medium, high, xhigh or max") != std::string::npos);
    CHECK_FALSE(parse({"--check-api", "--effort"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--effort", "Default"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--thinking", "default"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--thinking", "enabled"}, options, error));
    CHECK(error.find("adaptive, between_tools or disabled") != std::string::npos);
    CHECK_FALSE(parse({"--check-api", "--thinking"}, options, error));
    CHECK_FALSE(parse({"--check-api", "--thinking", "--effort", "low"}, options, error));
    // They belong to the two modes that send requests.
    CHECK_FALSE(parse({"--effort", "low"}, options, error));
    CHECK_FALSE(parse({"--thinking", "disabled"}, options, error));
    CHECK_FALSE(parse({"--watch-log", "a.log", "--effort", "low"}, options, error));
    CHECK(error.find("--effort") != std::string::npos);
    // Even the word that sends nothing: the flag was given to a mode that sends no request.
    CHECK_FALSE(parse({"--watch-log", "a.log", "--effort", "default"}, options, error));
    CHECK(error.find("--effort") != std::string::npos);
    CHECK_FALSE(parse({"--effort", "default"}, options, error));
    CHECK_FALSE(parse({"--smoke", "--effort", "default"}, options, error));
    // The default value itself is no flag: the other modes are unaffected.
    CHECK(parse({"--watch-log", "a.log"}, options, error));
    CHECK(parse({"--version"}, options, error));
    CHECK(parse({"--smoke"}, options, error));
    CHECK(parse({}, options, error));
    // The usage text names them, the default and the word that sends none.
    CHECK(curia::cliUsage().find("--effort <level>") != std::string_view::npos);
    CHECK(curia::cliUsage().find("--thinking <mode>") != std::string_view::npos);
    CHECK(curia::cliUsage().find("Without it Curia sends low") != std::string_view::npos);
    CHECK(curia::cliUsage().find("--effort default sends none") != std::string_view::npos);
}

TEST_CASE("an unknown positional word is never repeated in the error") {
    curia::CliOptions options;
    std::string error;
    const std::string stray = std::string("sk-ant-") + "FAKE0123456789abcdefgh";
    CHECK_FALSE(parse({stray.c_str()}, options, error));
    CHECK(error.find("FAKE") == std::string::npos);
    CHECK_FALSE(parse({"--nope"}, options, error));
    CHECK(error.find("--nope") != std::string::npos);
    const std::string flagLike = "--" + stray;
    CHECK_FALSE(parse({flagLike.c_str()}, options, error));
    CHECK(error.find("FAKE0123") == std::string::npos);
}

TEST_CASE("the chat window takes --log, --test-endpoint, --auth and --log-dir") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({}, options, error));
    CHECK(options.windowMode());
    CHECK(parse({"--log", "d.log"}, options, error));
    CHECK(options.windowMode());
    CHECK((options.logPath == "d.log"));
    CHECK(parse({"--log", "d.log", "--test-endpoint", "http://127.0.0.1:9", "--auth", "x-api-key",
                 "--log-dir", "logs"},
                options, error));
    CHECK((options.testEndpoint == "http://127.0.0.1:9"));
    CHECK(options.auth == curia::AuthMode::XApiKey);
    CHECK((options.appLogDir == "logs"));
    CHECK_FALSE(parse({"--log"}, options, error));
    CHECK_FALSE(parse({"--log", ""}, options, error));
    // The request settings stay with the modes that print what they sent.
    CHECK_FALSE(parse({"--effort", "low"}, options, error));
    CHECK_FALSE(parse({"--log", "d.log", "--from-start"}, options, error));
    CHECK_FALSE(parse({"--log", "d.log", "--frame", "2"}, options, error));
    // The other modes are no window.
    CHECK(parse({"--smoke"}, options, error));
    CHECK_FALSE(options.windowMode());
    CHECK(parse({"--idle-test", "2"}, options, error));
    CHECK_FALSE(options.windowMode());
    CHECK(curia::cliUsage().find("--log <path>") != std::string_view::npos);
}

TEST_CASE("--screenshot and --window-size belong to --smoke") {
    curia::CliOptions options;
    std::string error;
    CHECK(
        parse({"--smoke", "--screenshot", "shot.bmp", "--window-size", "380x520"}, options, error));
    CHECK((options.screenshotPath == "shot.bmp"));
    CHECK(options.windowWidth == 380);
    CHECK(options.windowHeight == 520);
    CHECK(parse({"--smoke", "--window-size", "1200X900"}, options, error));
    CHECK(options.windowWidth == 1200);
    CHECK(options.windowHeight == 900);
    CHECK(parse({"--smoke"}, options, error));
    CHECK(options.screenshotPath.empty());
    CHECK(options.windowWidth == 0);
    // Bad sizes.
    for (const char* bad : {"380", "x520", "380x", "380x520x2", "100x520", "380x99999", "axb",
                            "-380x520", "380 x 520"}) {
        CAPTURE(bad);
        CHECK_FALSE(parse({"--smoke", "--window-size", bad}, options, error));
        CHECK(error.find("--window-size") != std::string::npos);
    }
    CHECK_FALSE(parse({"--smoke", "--window-size"}, options, error));
    CHECK_FALSE(parse({"--smoke", "--screenshot"}, options, error));
    // Anywhere else: a usage error (exit 2).
    CHECK_FALSE(parse({"--screenshot", "shot.bmp"}, options, error));
    CHECK(error.find("--smoke") != std::string::npos);
    CHECK_FALSE(parse({"--window-size", "380x520"}, options, error));
    CHECK_FALSE(parse({"--idle-test", "3", "--screenshot", "shot.bmp"}, options, error));
    CHECK_FALSE(
        parse({"--headless-chat", "--log", "d.log", "--window-size", "380x520"}, options, error));
}

TEST_CASE("--smoke-scene picks what --smoke draws") {
    curia::CliOptions options;
    std::string error;
    CHECK(parse({"--smoke"}, options, error));
    CHECK(options.smokeScene == curia::SmokeScene::Conversation);
    CHECK(parse({"--smoke", "--smoke-scene", "notices"}, options, error));
    CHECK(options.smokeScene == curia::SmokeScene::Notices);
    CHECK(parse({"--smoke-scene", "start", "--smoke", "--screenshot", "s.bmp"}, options, error));
    CHECK(options.smokeScene == curia::SmokeScene::Start);
    CHECK(parse({"--smoke", "--smoke-scene", "conversation"}, options, error));
    CHECK(options.smokeScene == curia::SmokeScene::Conversation);
    for (const char* bad : {"Notices", "", "thinking", "notices "}) {
        CAPTURE(bad);
        CHECK_FALSE(parse({"--smoke", "--smoke-scene", bad}, options, error));
        CHECK(error.find("--smoke-scene") != std::string::npos);
    }
    CHECK_FALSE(parse({"--smoke", "--smoke-scene"}, options, error));
    // Without --smoke: a usage error (exit 2).
    CHECK_FALSE(parse({"--smoke-scene", "notices"}, options, error));
    CHECK(error.find("--smoke") != std::string::npos);
    CHECK_FALSE(parse({"--idle-test", "3", "--smoke-scene", "start"}, options, error));
}

TEST_CASE("--window: the overlay panel is the default where it exists") {
    using curia::OverlayFocus;
    using curia::WindowKind;
    curia::CliOptions options;
    std::string error;
    // macOS: the panel, made the key window when it is opened.
    CHECK(parseOn(true, {}, options, error));
    CHECK((options.window == WindowKind::Overlay));
    CHECK(options.usesPanel());
    CHECK_FALSE(options.windowGiven);
    CHECK((options.overlayFocus == OverlayFocus::Key));
    CHECK(parseOn(true, {"--window", "plain"}, options, error));
    CHECK((options.window == WindowKind::Plain));
    CHECK(options.windowGiven);
    CHECK(parseOn(true, {"--window", "overlay", "--log", "d.log"}, options, error));
    CHECK((options.window == WindowKind::Overlay));
    // Windows and Linux: the normal window, and asking for the panel is a usage error.
    CHECK(parseOn(false, {}, options, error));
    CHECK((options.window == WindowKind::Plain));
    CHECK_FALSE(options.usesPanel());
    CHECK(parseOn(false, {"--window", "plain"}, options, error));
    CHECK_FALSE(parseOn(false, {"--window", "overlay"}, options, error));
    CHECK(error.find("macOS") != std::string::npos);
    // Bad values, and other modes.
    for (const char* bad : {"Overlay", "", "panel"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseOn(true, {"--window", bad}, options, error));
        CHECK(error.find("--window") != std::string::npos);
    }
    CHECK_FALSE(parseOn(true, {"--window"}, options, error));
    CHECK_FALSE(parseOn(true, {"--smoke", "--window", "plain"}, options, error));
    CHECK_FALSE(parseOn(true, {"--idle-test", "3", "--window", "overlay"}, options, error));
    CHECK_FALSE(
        parseOn(true, {"--headless-chat", "--log", "d.log", "--window", "plain"}, options, error));
}

TEST_CASE("--overlay-focus: the overlay panel of the chat window only") {
    using curia::OverlayFocus;
    curia::CliOptions options;
    std::string error;
    CHECK(parseOn(true, {"--overlay-focus", "click"}, options, error));
    CHECK((options.overlayFocus == OverlayFocus::Click));
    CHECK(options.overlayFocusGiven);
    CHECK(parseOn(true, {"--overlay-focus", "activate", "--window", "overlay"}, options, error));
    CHECK((options.overlayFocus == OverlayFocus::Activate));
    CHECK(parseOn(true, {"--overlay-focus", "key"}, options, error));
    CHECK((options.overlayFocus == OverlayFocus::Key));
    for (const char* bad : {"Key", "", "none", "keys"}) {
        CAPTURE(bad);
        CHECK_FALSE(parseOn(true, {"--overlay-focus", bad}, options, error));
        CHECK(error.find("--overlay-focus") != std::string::npos);
    }
    CHECK_FALSE(parseOn(true, {"--overlay-focus"}, options, error));
    // Not with the plain window, not in another mode, not where the panel does not exist.
    CHECK_FALSE(parseOn(true, {"--window", "plain", "--overlay-focus", "key"}, options, error));
    CHECK(error.find("--window plain") != std::string::npos);
    CHECK_FALSE(parseOn(true, {"--smoke", "--overlay-focus", "click"}, options, error));
    CHECK_FALSE(parseOn(true, {"--idle-test", "2", "--overlay-focus", "click"}, options, error));
    CHECK_FALSE(parseOn(false, {"--overlay-focus", "click"}, options, error));
    CHECK(error.find("macOS") != std::string::npos);
}

TEST_CASE("--overlay draws --smoke and --idle-test in the panel, asking for no keys") {
    using curia::OverlayFocus;
    using curia::WindowKind;
    curia::CliOptions options;
    std::string error;
    CHECK(parseOn(true, {"--smoke"}, options, error));
    CHECK((options.window == WindowKind::Plain));
    CHECK(parseOn(true,
                  {"--smoke", "--overlay", "--screenshot", "s.bmp", "--window-size", "380x520"},
                  options, error));
    CHECK(options.overlay);
    CHECK(options.usesPanel());
    CHECK((options.overlayFocus == OverlayFocus::Click));
    CHECK(parseOn(true, {"--overlay", "--idle-test", "3"}, options, error));
    CHECK(options.usesPanel());
    CHECK((options.overlayFocus == OverlayFocus::Click));
    CHECK(parseOn(true, {"--idle-test", "3"}, options, error));
    CHECK_FALSE(options.usesPanel());
    // Only with the two test modes, and only where the panel exists.
    CHECK_FALSE(parseOn(true, {"--overlay"}, options, error));
    CHECK(error.find("--overlay needs --smoke or --idle-test") != std::string::npos);
    CHECK_FALSE(parseOn(true, {"--watch-log", "a.log", "--overlay"}, options, error));
    CHECK_FALSE(parseOn(false, {"--smoke", "--overlay"}, options, error));
    CHECK(error.find("macOS") != std::string::npos);
    CHECK(curia::cliUsage().find("--overlay-focus") != std::string_view::npos);
    CHECK(curia::cliUsage().find("--window overlay|plain") != std::string_view::npos);
}

TEST_CASE("--selftest-hotkey is a mode of its own, on macOS") {
    curia::CliOptions options;
    std::string error;
    CHECK(parseOn(true, {"--selftest-hotkey"}, options, error));
    CHECK(options.selftestHotkey);
    CHECK_FALSE(options.windowMode());
    CHECK_FALSE(options.usesPanel());
    CHECK_FALSE(parseOn(false, {"--selftest-hotkey"}, options, error));
    CHECK_FALSE(parseOn(true, {"--selftest-hotkey", "--smoke"}, options, error));
    CHECK_FALSE(parseOn(true, {"--selftest-hotkey", "--log", "d.log"}, options, error));
}

TEST_CASE("the window kind and the focus mode have short names for the app log") {
    CHECK(std::string_view(curia::toString(curia::WindowKind::Overlay)) == "overlay");
    CHECK(std::string_view(curia::toString(curia::WindowKind::Plain)) == "plain");
    CHECK(std::string_view(curia::toString(curia::OverlayFocus::Key)) == "key");
    CHECK(std::string_view(curia::toString(curia::OverlayFocus::Click)) == "click");
    CHECK(std::string_view(curia::toString(curia::OverlayFocus::Activate)) == "activate");
}
