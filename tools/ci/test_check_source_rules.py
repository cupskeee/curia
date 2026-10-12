# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/check_source_rules.sh. Each test plants violations in a temporary tree (the script takes the
root directory as its argument) and checks the exit code and the message; the last test runs the script on the real
repository. Model IDs and variable names are assembled from parts so this file does not trip the hygiene scan."""
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "check_source_rules.sh"
REPO = SCRIPT.parent.parent.parent

MODEL_RULE = "model-ID literal"
CURL_RULE = "forbidden libcurl option"
REVEAL_RULE = "reveal() outside the allowlist"
ENV_RULE = "CURIA_ environment variable"


def run(files, root=None):
    """Write `files` (relative path -> text) into a fresh temp tree and run the script on it."""
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in files.items():
            path = pathlib.Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return run_on(tmp)


def run_on(root):
    done = subprocess.run(["bash", str(SCRIPT), str(root)], capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def model(*parts):
    return "".join(parts)


class ModelIdRule(unittest.TestCase):
    def assertFlagged(self, literal, path="app/src/x.cpp"):
        code, out = run({path: 'const char* m = "%s";\n' % literal})
        self.assertEqual(code, 1, (literal, out))
        self.assertIn(MODEL_RULE, out)
        self.assertIn(path, out)

    def assertClean(self, text, path="app/src/x.cpp"):
        code, out = run({path: text})
        self.assertEqual(code, 0, (text, out))
        self.assertIn("source rules ok", out)

    def test_each_family_is_flagged(self):
        for literal in [
            model("claude-", "sonnet-5-5"),
            model("claude-", "opus-4"),
            model("claude-", "haiku-5-5"),
            model("claude-", "instant-1"),
            model("claude-", "fable-1"),
            model("claude-", "3-5-sonnet"),
            model("claude-", "v1"),
            model("gpt-", "4"),
            model("gpt-", "4o-mini"),
            model("gemini-", "2.0-flash"),
            model("llama-", "3"),
            model("llama", "3.1"),
            model("deepseek-", "chat"),
            model("deepseek-", "reasoner"),
            model("mistral-", "large"),
            model("mistral-", "small-latest"),
            model("mistral-", "medium"),
            "sonnet" + model("-20", "250101"),
            model("some-model-20", "250929"),
        ]:
            with self.subTest(literal=literal):
                self.assertFlagged(literal)

    def test_case_insensitive(self):
        self.assertFlagged(model("Claude-", "Opus-4"))
        self.assertFlagged(model("GPT-", "4"))
        self.assertFlagged(model("CLAUDE-", "SONNET-5"))

    def test_no_exemption_for_tests_or_headers_or_comments(self):
        for path in ["app/tests/test_x.cpp", "app/src/x.h", "app/src/x.hpp", "app/src/y.mm", "app/src/z.cc",
                     "app/src/w.c", "app/tools/t.cpp"]:
            with self.subTest(path=path):
                self.assertFlagged(model("claude-", "sonnet-5"), path)
        code, out = run({"app/src/x.cpp": "// we use " + model("claude-", "opus-4") + " here\n"})
        self.assertEqual(code, 1, out)

    def test_neutral_names_and_lookalikes_pass(self):
        self.assertClean('const char* a = "mock-model-1";\n')
        self.assertClean("// claude and gpt and gemini are mentioned without an id\n")
        self.assertClean('const char* d = "2026-10-12"; int x = 20250101; auto s = "a-2025";\n')
        self.assertClean('const char* v = "msg_mock0001";\n')

    def test_date_suffix_needs_eight_digits_then_a_boundary(self):
        self.assertClean('const char* a = "x-2025010";\n')           # seven digits
        self.assertClean('const char* a = "x-202501011";\n')          # nine digits
        self.assertFlagged("x-20250101")                              # end of literal
        code, out = run({"app/src/x.cpp": "int a = x-20250101;\n"})   # end of line after the digits
        self.assertEqual(code, 1, out)

    def test_only_cpp_extensions_under_app_are_scanned(self):
        literal = model("claude-", "sonnet-5")
        for path in ["docs/a.md", "tools/x.py", "app/notes.txt", "mod/curia/x.txt", "README.md",
                     "app/fuzz/corpus/seed", "tests/fixtures/a.cpp"]:
            with self.subTest(path=path):
                code, out = run({path: literal + "\n", "app/src/ok.cpp": "int x;\n"})
                self.assertEqual(code, 0, (path, out))


class CurlRule(unittest.TestCase):
    def test_forbidden_options_are_flagged(self):
        for option in [
            "CURLOPT_VERBOSE",
            "CURLOPT_DEBUGFUNCTION",
            "CURLOPT_DEBUGDATA",
            "CURLOPT_STDERR",
            "CURLOPT_TRACE_" + "ASCII",
            "CURLOPT_TRACE_" + "CONFIG",
            "CURLOPT_SSL_" + "VERIFYPEER",
            "CURLOPT_SSL_" + "VERIFYHOST",
            "CURLOPT_PROXY_SSL_" + "VERIFYPEER",
            "CURLOPT_PROXY_SSL_" + "VERIFYHOST",
            "CURLSSLOPT_" + "NO_REVOKE",
            "CURLSSLOPT_NO_" + "REVOKE | CURLSSLOPT_REVOKE_BEST_EFFORT",
        ]:
            with self.subTest(option=option):
                code, out = run({"app/src/http/client.cpp": "curl_easy_setopt(h, %s, 1L);\n" % option})
                self.assertEqual(code, 1, out)
                self.assertIn(CURL_RULE, out)

    def test_allowed_options_pass(self):
        text = (
            "curl_easy_setopt(h, CURLOPT_URL, u);\n"
            "curl_easy_setopt(h, CURLOPT_CAINFO, ca);\n"
            "curl_easy_setopt(h, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_REVOKE_BEST_EFFORT));\n"
            "curl_easy_setopt(h, CURLOPT_ERRORBUFFER, buf);\n"
            "curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);\n"
        )
        code, out = run({"app/tools/https_smoke.cpp": text})
        self.assertEqual(code, 0, out)

    def test_not_exempt_in_tests_or_comments(self):
        code, out = run({"app/tests/t.cpp": "// CURLOPT_VERBOSE would print the headers\n"})
        self.assertEqual(code, 1, out)


class RevealRule(unittest.TestCase):
    CALL = "auto v = key.reveal();\n"

    def test_flagged_outside_the_allowlist(self):
        for path in ["app/src/ui/chat.cpp", "app/src/core/app_log.cpp", "app/src/core/dev_config.cpp",
                     "app/src/net/endpoint_policy.cpp", "app/src/providers/openai_provider.cpp",
                     "app/src/core/secretive.cpp", "app/src/httpx/client.cpp", "app/src/main.cpp",
                     "app/tools/x.cpp"]:
            with self.subTest(path=path):
                code, out = run({path: self.CALL})
                self.assertEqual(code, 1, out)
                self.assertIn(REVEAL_RULE, out)
                self.assertIn(path, out)

    def test_allowed_in_the_allowlist(self):
        for path in ["app/src/core/secret.cpp", "app/src/core/secret.h", "app/src/core/redact.cpp",
                     "app/src/core/redact.h", "app/src/http/client.cpp", "app/src/http/sub/deep.cpp",
                     "app/src/providers/anthropic_provider.cpp", "app/src/providers/anthropic_request.h",
                     "app/tests/test_secret.cpp"]:
            with self.subTest(path=path):
                code, out = run({path: self.CALL})
                self.assertEqual(code, 0, out)

    def test_spellings(self):
        for text in ["x.reveal()", "x->reveal()", "reveal()", "x.reveal ()", "x.reveal(\n)", "(reveal)(); y.reveal( )"]:
            with self.subTest(text=text):
                code, out = run({"app/src/a.cpp": text + "\n"})
                self.assertEqual(code, 1, out)
        # Words that merely contain the name are not calls of it.
        for text in ["revealed(x);", "unreveal(x);", "x.revealAll();", "int reveal_count = 0;", "auto reveal = 1;"]:
            with self.subTest(text=text):
                code, out = run({"app/src/a.cpp": text + "\n"})
                self.assertEqual(code, 0, out)


class EnvRule(unittest.TestCase):
    KEY = "CURIA_" + "ANTHROPIC_API_KEY"
    MODEL = "CURIA_" + "MODEL"

    def test_flagged_outside_dev_config(self):
        for text in [
            'auto v = std::getenv("%s");' % self.KEY,
            'auto v = getEnv("CURIA_%s");' % "ANYTHING",
            'auto v = platform::getEnv( "CURIA_%s" );' % "LOG_CONTENT",
            'const char* a = "%s";' % self.MODEL,
            'const char* a = "see %s for details";' % self.KEY,
            'puts("set %s first");' % self.MODEL,
        ]:
            with self.subTest(text=text):
                code, out = run({"app/src/core/other.cpp": text + "\n"})
                self.assertEqual(code, 1, out)
                self.assertIn(ENV_RULE, out)

    def test_allowed_in_dev_config_cpp_and_tests(self):
        text = 'auto v = getenv("%s"); const char* m = "%s";\n' % (self.KEY, self.MODEL)
        for path in ["app/src/core/dev_config.cpp", "app/tests/test_dev_config.cpp", "app/tests/sub/x.cpp"]:
            with self.subTest(path=path):
                code, out = run({path: text})
                self.assertEqual(code, 0, out)

    def test_dev_config_header_and_lookalike_files_are_not_exempt(self):
        text = 'const char* m = "%s";\n' % self.MODEL
        for path in ["app/src/core/dev_config.h", "app/src/core/dev_config.cpp.bak.cpp",
                     "app/src/core/old/dev_config.cpp"]:
            with self.subTest(path=path):
                code, out = run({path: text})
                self.assertEqual(code, 1, out)

    def test_comments_that_name_a_variable_without_a_literal_pass(self):
        # Documented choice: comments are not stripped; only string literals and getenv calls match.
        text = "// the key comes from %s and the model from %s\n" % (self.KEY, self.MODEL)
        code, out = run({"app/src/core/other.cpp": text})
        self.assertEqual(code, 0, out)

    def test_a_comment_with_a_getenv_call_is_flagged(self):
        # ... and a comment that contains a quoted name or a getenv call is a hit, by the same rule.
        code, out = run({"app/src/core/other.cpp": '// was: getenv("CURIA_%s")\n' % "X"})
        self.assertEqual(code, 1, out)


class Behaviour(unittest.TestCase):
    def test_clean_tree_passes(self):
        code, out = run({
            "app/src/a.cpp": "int main() { return 0; }\n",
            "app/src/a.h": "#pragma once\n",
            "app/tests/t.cpp": "int t;\n",
        })
        self.assertEqual(code, 0, out)
        self.assertIn("source rules ok", out)

    def test_missing_app_folder_is_an_error(self):
        # A wrong root must not make every rule a silent no-op.
        code, out = run({"README.md": "hello\n"})
        self.assertEqual(code, 2, out)
        self.assertIn("no app/ directory", out)

    def test_all_violations_are_reported_together(self):
        code, out = run({
            "app/src/a.cpp": '"%s"\n' % model("claude-", "sonnet-5"),
            "app/src/b.cpp": "curl_easy_setopt(h, CURLOPT_VERBOSE, 1L);\n",
            "app/src/c.cpp": "k.reveal();\n",
            "app/src/d.cpp": 'getenv("CURIA_%s");\n' % "A",
        })
        self.assertEqual(code, 1)
        for rule in [MODEL_RULE, CURL_RULE, REVEAL_RULE, ENV_RULE]:
            self.assertIn(rule, out)
        for name in ["app/src/a.cpp", "app/src/b.cpp", "app/src/c.cpp", "app/src/d.cpp"]:
            self.assertIn(name, out)

    def test_paths_with_spaces(self):
        code, out = run({"app/src/with space/a.cpp": 'x = "%s";\n' % model("gpt-", "4")})
        self.assertEqual(code, 1, out)
        self.assertIn("with space", out)

    def test_bad_root_is_an_error(self):
        code, out = run_on("/nonexistent/for/sure")
        self.assertEqual(code, 2)
        self.assertIn("cannot enter", out)

    def test_the_repository_passes(self):
        code, out = run_on(REPO)
        self.assertEqual(code, 0, out)
        # With no argument the script finds the repository itself.
        done = subprocess.run(["bash", str(SCRIPT)], capture_output=True, text=True, cwd=tempfile.gettempdir())
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
