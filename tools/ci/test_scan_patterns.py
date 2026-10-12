# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/scan_patterns.sh. Each test builds a temporary git repository (the script scans what git
tracks), plants one kind of violation and checks the exit code and message. Key-shaped values are assembled from
parts at run time, so this file itself holds no key-shaped literal."""
import os
import pathlib
import shutil
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "scan_patterns.sh"

LETTERS = "abcdefghijklmnopqrstuvwxyz"
REAL_TAIL = (LETTERS + "0123456789")[:40]          # no FAKE, no fake
ANTHROPIC = "sk-" + "ant-"
KEY_MSG = "key-shaped"
ID_MSG = "request id"
HEADER_MSG = "header"
PATH_MSG = "transcripts"


def scan(files, extra_tracked=()):
    """Create a git repo holding the scan script and `files`, run the script, return (exit code, output)."""
    with tempfile.TemporaryDirectory() as tmp:
        root = pathlib.Path(tmp)
        (root / "tools" / "ci").mkdir(parents=True)
        shutil.copy(SCRIPT, root / "tools" / "ci" / "scan_patterns.sh")
        for rel, text in files.items():
            path = root / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        env = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_SYSTEM=os.devnull)
        subprocess.run(["git", "init", "-q"], cwd=root, check=True, env=env)
        subprocess.run(["git", "add", "-A"], cwd=root, check=True, env=env)
        done = subprocess.run(["bash", "tools/ci/scan_patterns.sh"], cwd=root, capture_output=True, text=True,
                              env=env)
        return done.returncode, done.stdout + done.stderr


class KeyShapes(unittest.TestCase):
    def assertFails(self, text, message, path="app/src/a.cpp"):
        code, out = scan({path: text})
        self.assertEqual(code, 1, (text, out))
        self.assertIn(message, out)

    def assertPasses(self, text, path="app/src/a.cpp"):
        code, out = scan({path: text})
        self.assertEqual(code, 0, (text, out))
        self.assertIn("scan ok", out)

    def test_clean_repository_passes(self):
        self.assertPasses("int main() { return 0; }\n")

    def test_real_looking_anthropic_key_fails(self):
        self.assertFails('const char* k = "%s%s";\n' % (ANTHROPIC, REAL_TAIL), KEY_MSG)

    def test_fake_key_passes_whichever_case_is_in_the_token(self):
        self.assertPasses('k = "%sFAKE%s";\n' % (ANTHROPIC, REAL_TAIL))
        self.assertPasses('k = "%sfake%s";\n' % (ANTHROPIC, REAL_TAIL))
        self.assertPasses('k = "%s%sFAKE";\n' % (ANTHROPIC, REAL_TAIL))

    def test_a_fake_word_elsewhere_on_the_line_does_not_exempt_a_real_key(self):
        self.assertFails('// fake example, real key: %s%s\n' % (ANTHROPIC, REAL_TAIL), KEY_MSG)
        self.assertFails('k = "%s%s"; // FAKE\n' % (ANTHROPIC, REAL_TAIL), KEY_MSG)
        self.assertFails('fakefakefake %s%s\n' % (ANTHROPIC, REAL_TAIL), KEY_MSG)

    def test_one_fake_and_one_real_token_on_a_line_fails(self):
        self.assertFails('a = "%sFAKE%s"; b = "%s%s";\n' % (ANTHROPIC, REAL_TAIL, ANTHROPIC, REAL_TAIL), KEY_MSG)

    def test_short_anthropic_prefix_is_not_a_key(self):
        self.assertPasses('k = "%sshort";\n' % ANTHROPIC)

    def test_other_key_shapes(self):
        self.assertFails("k = sk-%s\n" % ("A" * 40), KEY_MSG)
        self.assertFails("k = AKIA%s\n" % ("B" * 16), KEY_MSG)
        self.assertFails("k = AIza%s\n" % ("c" * 35), KEY_MSG)
        self.assertPasses("k = AKIA%sFAKE%s\n" % ("B" * 6, "B" * 6))

    def test_real_request_ids_fail_and_synthetic_ones_pass(self):
        for prefix in ["msg_", "req_", "toolu_"]:
            with self.subTest(prefix=prefix):
                self.assertFails('id = "%s01%s";\n' % (prefix, REAL_TAIL[:24]), ID_MSG)
                self.assertPasses('id = "%s01%sFAKE";\n' % (prefix, REAL_TAIL[:24]))
        self.assertPasses('id = "msg_mock0001"; r = "req_mock0002"; t = "toolu_mock0003";\n')
        self.assertPasses('id = "msg_01short";\n')

    def test_header_with_a_long_literal_token_fails_outside_docs(self):
        token = REAL_TAIL[:24]
        for text in ["x-api-key: " + token, "X-API-KEY:" + token, '"x-api-key": "%s"' % token,
                     "Authorization: Bearer " + token, "authorization:bearer   " + token,
                     '"Authorization": "Bearer %s"' % token]:
            with self.subTest(text=text):
                self.assertFails(text + "\n", HEADER_MSG)
                self.assertFails(text + "\n", HEADER_MSG, path="tools/m4/x.py")

    def test_header_token_length_boundary(self):
        for head in ["x-api-key: ", "Authorization: Bearer "]:
            with self.subTest(head=head):
                self.assertFails(head + REAL_TAIL[:20] + "\n", HEADER_MSG)
                self.assertPasses(head + REAL_TAIL[:19] + "\n")

    def test_header_rule_exemptions(self):
        token = REAL_TAIL[:24]
        self.assertPasses("x-api-key: %s\n" % token, path="docs/api.md")
        self.assertPasses("Authorization: Bearer %s\n" % token, path="docs/api.md")
        self.assertPasses("x-api-key: %sFAKE\n" % token)
        self.assertPasses("Authorization: Bearer fake%s\n" % token)
        self.assertPasses("x-api-key: " + REAL_TAIL[:19] + "\n")         # 19 characters
        self.assertPasses('headers["x-api-key"] = key;\n')
        self.assertPasses("Authorization: Bearer <token>\n")

    def test_transcript_paths_fail(self):
        for path in ["transcripts/a.txt", "docs/transcripts/b.txt", "curia_m4_results/c.json",
                     "tools/curia_m4_results/transcripts/d.txt", "a/b/transcripts"]:
            with self.subTest(path=path):
                code, out = scan({path: "x\n"})
                self.assertEqual(code, 1, out)
                self.assertIn(PATH_MSG, out)
                self.assertIn(path, out)

    def test_similar_paths_pass(self):
        for path in ["transcripts_old/a.txt", "my_transcripts.txt", "docs/transcript/a.txt",
                     "curia_m4_results_notes.md", "xtranscripts/a.txt"]:
            with self.subTest(path=path):
                code, out = scan({path: "x\n"})
                self.assertEqual(code, 0, out)

    def test_existing_rules_still_fire(self):
        code, out = scan({"reference/dump.txt": "x\n"})
        self.assertEqual(code, 1)
        self.assertIn("reference/", out)
        code, out = scan({"a.txt": "see /Us" + "ers/someone/file\n"})
        self.assertEqual(code, 1)
        self.assertIn("home path", out)
        code, out = scan({"a.txt": "[12:34:56][D]" + "[x.cpp:1]: text\n"})
        self.assertEqual(code, 1)
        self.assertIn("engine log line", out)

    def test_the_repository_itself_passes(self):
        repo = SCRIPT.parent.parent.parent
        done = subprocess.run(["bash", str(SCRIPT)], cwd=repo, capture_output=True, text=True)
        # Run on the working tree's index: files added with `git add -N` are scanned; untracked ones are not.
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)


if __name__ == "__main__":
    unittest.main()
