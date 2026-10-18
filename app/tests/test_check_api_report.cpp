// SPDX-License-Identifier: MIT
#include <doctest/doctest.h>

#include <string>
#include <vector>

#include "core/check_api_report.h"
#include "core/dev_config.h"

using curia::AuthMode;
using curia::CheckApiReport;
using curia::CheckStep;
using curia::providers::ChatError;
using curia::providers::Usage;

namespace {

bool anyLineContains(const std::vector<std::string>& lines, const std::string& part) {
    for (const std::string& line : lines) {
        if (line.find(part) != std::string::npos) {
            return true;
        }
    }
    return false;
}

ChatError error(ChatError::Code code, int status, const std::string& message = "") {
    ChatError result;
    result.code = code;
    result.httpStatus = status;
    result.message = message;
    return result;
}

}  // namespace

TEST_CASE("check-api report: layout and summary line") {
    CheckApiReport report;
    CheckStep ok;
    ok.name = "API key";
    ok.detail = "present (never shown)";
    report.add(ok);
    CheckStep bad;
    bad.name = "Tiny answer";
    bad.state = CheckStep::State::Failed;
    bad.detail = "no answer";
    bad.advice = {"Check the model."};
    report.add(bad);
    report.set("key", "present");
    report.set("model", "");
    report.set("status", "404");
    const std::string text = curia::formatCheckApiReport(report);
    CHECK(text.find("[ok] API key: present (never shown)\n") == 0);
    CHECK(text.find("[FAILED] Tiny answer: no answer\n       -> Check the model.\n") !=
          std::string::npos);
    CHECK(text.find("\nCHECK_API_RESULT ok=false key=present model=- status=404\n") !=
          std::string::npos);
    CHECK_FALSE(report.passed());
    CHECK(curia::checkApiSummaryLine(report) ==
          "CHECK_API_RESULT ok=false key=present model=- status=404");
}

TEST_CASE("check-api report: info and skipped steps do not fail the report") {
    CheckApiReport report;
    CheckStep info;
    info.name = "Cache";
    info.state = CheckStep::State::Info;
    report.add(info);
    CheckStep skipped;
    skipped.name = "Probe";
    skipped.state = CheckStep::State::Skipped;
    report.add(skipped);
    CHECK(report.passed());
    CHECK(curia::formatCheckApiReport(report).find("[info] Cache\n") == 0);
    CHECK(curia::formatCheckApiReport(report).find("[skipped] Probe\n") != std::string::npos);
}

TEST_CASE("check-api advice: each kind of failure has plain wording") {
    const std::vector<std::string> auth =
        curia::adviceForError(error(ChatError::Code::Auth, 401), AuthMode::Bearer);
    CHECK(anyLineContains(auth, "did not accept the key"));
    CHECK(anyLineContains(auth, "--auth x-api-key"));
    CHECK(anyLineContains(auth, "HTTP 401"));
    const std::vector<std::string> other =
        curia::adviceForError(error(ChatError::Code::Auth, 403), AuthMode::XApiKey);
    CHECK(anyLineContains(other, "--auth bearer"));

    const std::vector<std::string> notFound = curia::adviceForError(
        error(ChatError::Code::BadRequest, 404, "model not found: check the configured model"),
        AuthMode::Bearer);
    CHECK(anyLineContains(notFound, "model was not found"));
    CHECK(anyLineContains(notFound, std::string(curia::modelVariableName())));
    CHECK(anyLineContains(notFound, "model not found: check the configured model"));

    ChatError limited = error(ChatError::Code::RateLimit, 429);
    limited.retryAfterSeconds = 7;
    const std::vector<std::string> rate = curia::adviceForError(limited, AuthMode::Bearer);
    CHECK(anyLineContains(rate, "too many requests"));
    CHECK(anyLineContains(rate, "7 seconds"));

    const std::vector<std::string> network = curia::adviceForError(
        error(ChatError::Code::Network, 0, "could not resolve host"), AuthMode::Bearer);
    CHECK(anyLineContains(network, "VPN"));
    CHECK(anyLineContains(network, "could not resolve host"));
    CHECK(anyLineContains(
        curia::adviceForError(error(ChatError::Code::Overloaded, 529), AuthMode::Bearer), "busy"));
    CHECK(
        anyLineContains(curia::adviceForError(error(ChatError::Code::Other, 500), AuthMode::Bearer),
                        "service side"));
}

TEST_CASE("check-api advice: a refused request that carried effort or thinking says so") {
    using curia::SentSettings;
    using curia::providers::Effort;
    using curia::providers::Thinking;
    const ChatError refused =
        error(ChatError::Code::BadRequest, 400, "thinking.type: value not supported");
    // Without a setting (--effort default, no --thinking): the usual advice only.
    const std::vector<std::string> plain = curia::adviceForError(refused, AuthMode::Bearer);
    CHECK(anyLineContains(plain, "refused the request"));
    CHECK_FALSE(anyLineContains(plain, "The request carried"));
    CHECK_FALSE(anyLineContains(plain, "may not support"));
    // Curia's default effort and --thinking: the line names both, where each came from, the
    // likely cause and how to send neither.
    const std::vector<std::string> tuned =
        curia::adviceForError(refused, AuthMode::Bearer,
                              SentSettings{.effort = Effort::Low, .thinking = Thinking::Disabled});
    CHECK(anyLineContains(tuned, "refused the request"));
    CHECK(anyLineContains(
        tuned, "The request carried effort low (Curia's default) and --thinking disabled."));
    CHECK(anyLineContains(tuned, "may not support that effort or thinking setting"));
    CHECK(anyLineContains(tuned, "only at effort high or below"));
    CHECK(anyLineContains(tuned, "thinking.type: value not supported"));
    CHECK(anyLineContains(tuned, "Run the same check with --effort default and without --thinking "
                                 "(then no effort or thinking setting is sent) to see"));
    // Only the effort, Curia's default or a flag.
    const std::vector<std::string> byDefault =
        curia::adviceForError(refused, AuthMode::Bearer, SentSettings{.effort = Effort::Low});
    CHECK(anyLineContains(byDefault, "The request carried effort low (Curia's default)."));
    // Only an effort was sent, so the line speaks of the effort alone.
    CHECK(anyLineContains(byDefault, "The model may not support that effort setting. Run"));
    CHECK_FALSE(anyLineContains(byDefault, "effort or thinking"));
    CHECK_FALSE(anyLineContains(byDefault, "only on some models"));
    CHECK_FALSE(anyLineContains(byDefault, "--thinking"));
    CHECK(anyLineContains(
        byDefault,
        "Run the same check with --effort default (then no effort setting is sent) to see"));
    const std::vector<std::string> byFlag = curia::adviceForError(
        refused, AuthMode::Bearer, SentSettings{.effort = Effort::Medium, .effortFromFlag = true});
    CHECK(anyLineContains(byFlag, "The request carried --effort medium."));
    CHECK(anyLineContains(byFlag, "with --effort default (then no effort setting is sent)"));
    // Only the thinking (with --effort default).
    const std::vector<std::string> thinkingOnly = curia::adviceForError(
        refused, AuthMode::Bearer,
        SentSettings{
            .effort = Effort::Default, .effortFromFlag = true, .thinking = Thinking::BetweenTools});
    CHECK(anyLineContains(thinkingOnly, "The request carried --thinking between_tools."));
    CHECK(anyLineContains(thinkingOnly, "may not support that thinking setting (thinking disabled "
                                        "or between_tools works only on some models"));
    CHECK(anyLineContains(
        thinkingOnly,
        "Run the same check without --thinking (then no thinking setting is sent) to see"));
    CHECK_FALSE(anyLineContains(thinkingOnly, "--effort default"));
    // Adaptive thinking is no value some models refuse, so the note on those values is left out.
    const std::vector<std::string> adaptive = curia::adviceForError(
        refused, AuthMode::Bearer,
        SentSettings{
            .effort = Effort::High, .effortFromFlag = true, .thinking = Thinking::Adaptive});
    CHECK(anyLineContains(adaptive, "The request carried --effort high and --thinking adaptive."));
    CHECK(anyLineContains(adaptive, "may not support that effort or thinking setting. Run"));
    CHECK_FALSE(anyLineContains(adaptive, "only on some models"));
    // A model that is not found, or a request that is too large, is not about the settings.
    const std::vector<std::string> notFound =
        curia::adviceForError(error(ChatError::Code::BadRequest, 404), AuthMode::Bearer,
                              SentSettings{.effort = Effort::High, .effortFromFlag = true});
    CHECK_FALSE(anyLineContains(notFound, "The request carried"));
    const std::vector<std::string> tooLarge =
        curia::adviceForError(error(ChatError::Code::BadRequest, 413), AuthMode::Bearer,
                              SentSettings{.effort = Effort::High, .effortFromFlag = true});
    CHECK_FALSE(anyLineContains(tooLarge, "The request carried"));
}

TEST_CASE("check-api settings: the words for what the requests carry") {
    using curia::SentSettings;
    using curia::providers::Effort;
    using curia::providers::Thinking;
    CHECK(curia::describeSettings(SentSettings{}).empty());
    CHECK(curia::describeSettings(SentSettings{.effort = Effort::Default, .effortFromFlag = true})
              .empty());
    CHECK((curia::describeSettings(SentSettings{.effort = Effort::Low}) ==
           "effort low (Curia's default)"));
    CHECK((curia::describeSettings(SentSettings{.effort = Effort::Low, .effortFromFlag = true}) ==
           "--effort low"));
    CHECK((curia::describeSettings(SentSettings{.effort = Effort::Max, .effortFromFlag = true}) ==
           "--effort max"));
    CHECK((curia::describeSettings(SentSettings{.effort = Effort::Default,
                                                .effortFromFlag = true,
                                                .thinking = Thinking::BetweenTools}) ==
           "--thinking between_tools"));
    CHECK((curia::describeSettings(SentSettings{
               .effort = Effort::Medium, .effortFromFlag = true, .thinking = Thinking::Adaptive}) ==
           "--effort medium and --thinking adaptive"));

    // From the command line: no flag is Curia's default effort and no thinking setting.
    curia::CliOptions options;
    SentSettings settings = curia::sentSettings(options);
    CHECK((settings.effort == curia::providers::kDefaultEffort));
    CHECK_FALSE(settings.effortFromFlag);
    CHECK((settings.thinking == Thinking::Default));
    CHECK((curia::describeSettings(settings) == "effort low (Curia's default)"));
    options.effort = Effort::Default;
    options.effortGiven = true;
    options.thinking = Thinking::Disabled;
    options.thinkingGiven = true;
    settings = curia::sentSettings(options);
    CHECK((settings.effort == Effort::Default));
    CHECK(settings.effortFromFlag);
    CHECK((curia::describeSettings(settings) == "--thinking disabled"));
}

TEST_CASE("check-api advice: missing variables are named without values") {
    const std::vector<std::string> advice = curia::adviceForMissing(
        {std::string(curia::keyVariableName()), std::string(curia::modelVariableName())});
    REQUIRE(advice.size() == 2);
    CHECK(advice[0].find(std::string(curia::keyVariableName())) == 0);
    CHECK(advice[1].find(std::string(curia::modelVariableName())) == 0);
    CHECK(curia::adviceForMissing({}).empty());
}

TEST_CASE("check-api: the auth names") {
    CHECK(std::string(curia::authName(AuthMode::Bearer)) == "bearer");
    CHECK(std::string(curia::authName(AuthMode::XApiKey)) == "x-api-key");
}

TEST_CASE("cache probe verdict: hit, written but not read, miss") {
    Usage first;
    first.input = 20;
    first.cacheWrite = 9000;
    Usage second;
    second.input = 20;
    second.cacheRead = 9000;
    curia::CacheProbeVerdict verdict = curia::judgeCacheProbe(first, second);
    CHECK(verdict.result == "hit");
    CHECK(verdict.prefixTokens == 9020);
    CHECK(verdict.lines.front().find("cache hit") == 0);

    second.cacheRead = 0;
    verdict = curia::judgeCacheProbe(first, second);
    CHECK(verdict.result == "written_not_read");
    CHECK(anyLineContains(verdict.lines, "written but not read"));

    Usage nothing;
    nothing.input = 800;
    verdict = curia::judgeCacheProbe(nothing, nothing);
    CHECK(verdict.result == "miss");
    CHECK(verdict.prefixTokens == 800);
    CHECK(verdict.lines.front() ==
          "no cache hit: prefix below the model minimum or caching not applied");
    CHECK(anyLineContains(verdict.lines, "800 tokens in total"));
    CHECK(anyLineContains(verdict.lines, "first: input=800 cache_write=0 cache_read=0"));
}

TEST_CASE("cache probe text: deterministic, the asked size, plain ASCII") {
    const std::string a = curia::cacheProbeText();
    CHECK(a == curia::cacheProbeText());
    CHECK(a.size() == 32 * 1024);
    for (const char ch : a) {
        CHECK((ch == '\n' || (ch >= 0x20 && ch < 0x7f)));
    }
    CHECK(curia::cacheProbeText(500).size() == 500);
}
