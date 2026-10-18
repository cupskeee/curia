# SPDX-License-Identifier: MIT
"""Unit tests for tools/m4/grep_key.py (FAKE keys in temporary folders only).

Run: python3 -m unittest discover -s tools/m4 -v
"""
from __future__ import annotations

import base64
import contextlib
import gzip
import io
import json
import os
import subprocess
import sys
import tempfile
import unittest
import urllib.parse

import grep_key as gk

KEY = "sk-ant-FAKE-grep-test-0123456789-abcdefXYZ"
# A key with characters that JSON and URLs encode, so that every form differs from the raw one.
ODD_KEY = 'FAKE/odd+key="quoted"\\back slash'


def run_main(args, key=KEY, env=None):
    out, err = io.StringIO(), io.StringIO()
    environment = {gk.VARIABLE: key} if key is not None else {}
    environment.update(env or {})
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = gk.main(list(args), env=environment)
    return code, out.getvalue(), err.getvalue()


def pieces_of(key):
    """The key, each of its encodings and every 8-character piece of its text: none may be printed."""
    texts = [key] + [needle.decode("latin-1") for needle in gk.encodings(key)]
    texts += [key[i:i + 8] for i in range(0, len(key) - 7)]
    return texts


class Folder:
    def __init__(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.path = self._tmp.name

    def file(self, relative, data):
        path = os.path.join(self.path, relative)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as handle:
            handle.write(data if isinstance(data, bytes) else data.encode("utf-8"))
        return path

    def close(self):
        self._tmp.cleanup()


class GrepKeyTestCase(unittest.TestCase):
    def setUp(self):
        self.folder = Folder()
        self.addCleanup(self.folder.close)

    def assert_nothing_secret(self, key, *outputs):
        text = "".join(outputs)
        for piece in pieces_of(key):
            self.assertNotIn(piece, text)


class EncodingTests(GrepKeyTestCase):
    def test_every_form_is_found_in_its_own_file(self):
        raw = ODD_KEY.encode("utf-8")
        forms = {
            "raw": b"before " + raw + b" after",
            "json": json.dumps({"key": ODD_KEY}).encode("ascii"),
            "json_slash": json.dumps({"key": ODD_KEY}).replace("/", "\\/").encode("ascii"),
            "url": b"https://example.invalid/?k=" + urllib.parse.quote(ODD_KEY, safe="").encode("ascii"),
            "url_plus": b"k=" + urllib.parse.quote_plus(ODD_KEY, safe="").encode("ascii"),
            "utf16": b"\xff\xfe" + ("x" + ODD_KEY + "y").encode("utf-16-le"),
            "utf16be": ("x" + ODD_KEY).encode("utf-16-be"),
        }
        for shift in range(3):
            blob = b"z" * shift + raw + b"tail"
            forms["b64_%d" % shift] = b"data:" + base64.b64encode(b"PRE" + blob) + b"\n"
            forms["b64url_%d" % shift] = base64.urlsafe_b64encode(b"PRE" + blob)
        for name, data in forms.items():
            with self.subTest(form=name):
                path = self.folder.file(name + ".bin", data)
                code, out, err = run_main([path], key=ODD_KEY)
                self.assertEqual(code, 1, out + err)
                self.assertIn("KEY FOUND: %s.bin" % name, out)
                self.assert_nothing_secret(ODD_KEY, out, err)

    def test_forms_that_give_the_same_bytes_are_one_match(self):
        path = self.folder.file("one.txt", "x " + KEY + " y")
        code, out, _ = run_main([path])
        self.assertEqual(code, 1)
        self.assertIn("raw (also JSON-escaped, URL-encoded) 1", out)

    def test_base64_pieces_do_not_depend_on_the_bytes_around_them(self):
        raw = KEY.encode()
        pieces = gk._base64_pieces(raw)
        self.assertEqual(len(pieces), 3)
        for shift, piece in zip(range(3), pieces):
            for before in (b"", b"a", b"bc", b"def"):
                if len(before) % 3 != shift % 3:
                    continue
                for after in (b"", b"!", b"??"):
                    self.assertIn(piece, base64.b64encode(before + raw + after))

    def test_a_cut_copy_of_the_key_is_a_piece_of_it(self):
        cut = self.folder.file("cut.log", "x " + KEY[:30] + "...\n")
        code, out, err = run_main([cut])
        self.assertEqual(code, 1, out + err)
        self.assertIn("KEY FOUND: cut.log: a piece of the key (its first 24 characters) 1", out)
        self.assertNotIn("shape sk-ant- (not the key)", out)
        whole = self.folder.file("whole.log", "x " + KEY + " y " + KEY[:24])
        code, out, err = run_main([whole])
        self.assertEqual(code, 1)
        self.assertIn("KEY FOUND: whole.log: raw (also JSON-escaped, URL-encoded) 1, a piece of the key (its first 24 "
                      "characters) 1", out)
        short = "FAKE-short-key-0123456789"  # too short for a piece apart from the key
        self.assertIsNone(gk.key_piece(short))
        self.assert_nothing_secret(KEY, out, err)

    def test_what_is_not_looked_into(self):
        # Documented limits: compressed files and base64 split over lines.
        self.folder.file("old.log.gz", gzip.compress(("x " + KEY + "\n").encode()))
        self.folder.file("mail.txt", base64.encodebytes(b"x" * 40 + KEY.encode() + b"y" * 40))
        code, out, err = run_main([self.folder.path])
        self.assertEqual(code, 0, out + err)
        self.assertIn("Not looked into: compressed files, base64 split over lines.", out)

    def test_a_clean_file_and_a_binary_file(self):
        clean = self.folder.file("clean.log", "2026-10-20T19:00:00.000Z info ask_done gen=1\n")
        binary = self.folder.file("core.bin", os.urandom(4096) + b"\0\0" + KEY.encode() + b"\0" + os.urandom(64))
        code, out, err = run_main([clean])
        self.assertEqual(code, 0, out + err)
        self.assertIn("Result: the key was found nowhere (1 file(s) scanned).", out)
        code, out, err = run_main([binary])
        self.assertEqual(code, 1)
        self.assertIn("KEY FOUND: core.bin", out)
        self.assert_nothing_secret(KEY, out, err)


class ScanTests(GrepKeyTestCase):
    def test_a_key_across_piece_boundaries_is_counted_once_per_occurrence(self):
        raw = KEY.encode()
        data = b"".join(b"." * gap + raw for gap in (0, 1, 63, 64, 65, 200, 511, 513, 1000))
        path = self.folder.file("many.bin", data)
        needles = gk.encodings(KEY)
        for chunk in (16, 63, 64, 100, 1000, 1 << 20):
            with self.subTest(chunk=chunk):
                result = gk.scan_file(path, needles, list(needles), 1 << 30, chunk_bytes=chunk,
                                      piece=gk.key_piece(KEY))
                self.assertEqual(sum(result.key.values()), 9)  # whole keys, no piece apart from them
                self.assertNotIn(gk.PIECE_NAME, result.key)
                self.assertFalse(result.partial)

    def test_the_size_cap_is_reported_and_only_the_start_is_read(self):
        raw = KEY.encode()
        path = self.folder.file("huge.log", raw + b"x" * (3 * 1024 * 1024) + raw)
        code, out, err = run_main([path, "--max-bytes", str(1024 * 1024)])
        self.assertEqual(code, 1)
        self.assertIn("partly scanned (only the first 1 MiB): huge.log", out)
        self.assertIn("KEY FOUND: huge.log: raw (also JSON-escaped, URL-encoded) 1", out)  # the first only
        late = self.folder.file("late.log", b"x" * (2 * 1024 * 1024) + raw)
        code, out, err = run_main([late, "--max-bytes", str(1024 * 1024)])
        self.assertEqual(code, 3)  # not found in the part read: not a clean result
        self.assertIn("partly scanned", out)
        self.assertIn("Result: NOT COMPLETE: the key was not found in what could be read (1 file(s) scanned), but 0 "
                      "file(s) or folder(s) could not be read and 1 file(s) were read only in part", out)
        code, out, err = run_main([late])  # the default cap reads it whole
        self.assertEqual(code, 1)
        self.assertNotIn("partly scanned", out)
        self.assert_nothing_secret(KEY, out, err)

    def test_links_inside_a_folder_are_not_followed_a_given_link_is(self):
        outside = Folder()
        self.addCleanup(outside.close)
        secret = outside.file("secret.txt", KEY)
        os.makedirs(os.path.join(self.folder.path, "logs"))
        os.symlink(secret, os.path.join(self.folder.path, "logs", "to-secret"))
        os.symlink(outside.path, os.path.join(self.folder.path, "logs", "to-folder"))
        os.symlink(self.folder.path, os.path.join(self.folder.path, "logs", "loop"))
        self.folder.file("logs/plain.txt", "nothing")
        code, out, err = run_main([self.folder.path])
        self.assertEqual(code, 0, out + err)
        self.assertIn("links not followed: 3", out)
        code, out, err = run_main([os.path.join(self.folder.path, "logs", "to-secret")])
        self.assertEqual(code, 1)
        self.assertIn("(a link, followed)", out)
        self.assert_nothing_secret(KEY, out, err)

    @unittest.skipIf(not hasattr(os, "geteuid") or os.geteuid() == 0, "needs file permissions (not root)")
    def test_an_unreadable_file_is_listed_and_the_rest_is_scanned(self):
        locked = self.folder.file("locked.log", KEY)
        self.folder.file("open.log", "fine")
        os.chmod(locked, 0)
        self.addCleanup(os.chmod, locked, 0o600)
        code, out, err = run_main([self.folder.path])
        self.assertEqual(code, 3)
        self.assertIn("could not read: locked.log", out)
        self.assertIn("files scanned: 1", out)
        self.assertIn("Result: NOT COMPLETE", out)

    @unittest.skipIf(not hasattr(os, "geteuid") or os.geteuid() == 0, "needs file permissions (not root)")
    def test_a_folder_that_cannot_be_listed_is_not_a_clean_result(self):
        self.folder.file("closed/curia.log", KEY)
        closed = os.path.join(self.folder.path, "closed")
        os.chmod(closed, 0)
        self.addCleanup(os.chmod, closed, 0o700)
        code, out, err = run_main([closed])
        self.assertEqual(code, 3, out + err)
        self.assertIn("could not read: .", out)
        self.assertIn("files scanned: 0", out)
        self.assertNotIn("found nowhere", out)
        code, out, err = run_main([os.path.join(closed, "curia.log")])  # a path through it
        self.assertEqual(code, 3, out + err)
        self.assertIn("could not read: .", out)

    @unittest.skipIf(not hasattr(os, "mkfifo"), "no named pipes here")
    def test_a_named_pipe_is_skipped_not_opened(self):
        os.mkfifo(os.path.join(self.folder.path, "pipe"))
        self.folder.file("a.log", "fine")
        code, out, _ = run_main([self.folder.path])
        self.assertEqual(code, 0)
        self.assertIn("special files skipped: 1", out)

    def test_a_path_that_holds_the_key_is_not_printed(self):
        self.folder.file(os.path.join("sub", "x-" + KEY + ".txt"), "nothing inside")
        code, out, err = run_main([self.folder.path])
        self.assertEqual(code, 1)
        self.assertIn("[a path that contains the key; not shown]: in its path 1", out)
        self.assert_nothing_secret(KEY, out, err)

    def test_name_filter_inside_folders(self):
        self.folder.file("Curia-2026-10-20.ips", KEY)
        self.folder.file("OtherApp-2026-10-20.ips", KEY)
        code, out, _ = run_main([self.folder.path, "--name-contains", "curia"])
        self.assertEqual(code, 1)
        self.assertIn("KEY FOUND: Curia-2026-10-20.ips", out)
        self.assertNotIn("OtherApp", out)
        self.assertIn("files without 'curia' in the name skipped: 1", out)

    def test_other_key_shapes_are_counted_apart(self):
        self.folder.file("tests.log", "a sk-ant-FAKE-test-key-0000 b sk-ant-FAKE-another-1111")
        self.folder.file("odd.log", "sk-ant-abcdefgh12345678 and " + KEY)
        code, out, err = run_main([self.folder.path])
        self.assertEqual(code, 1)
        self.assertIn("shape sk-ant- (not the key): odd.log: 1", out)
        self.assertIn("shape sk-ant- with FAKE only (test keys): 1 file(s), 2 match(es)", out)
        self.assertNotIn("abcdefgh", out)
        self.assert_nothing_secret(KEY, out, err)


class ProcessTests(GrepKeyTestCase):
    """The tool as the sheet runs it: `python3 -I`, the key only in the environment."""

    def run_tool(self, args, key):
        script = os.path.join(os.path.dirname(os.path.abspath(__file__)), "grep_key.py")
        env = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "PYTHONUTF8": "1"}
        if key is not None:
            env[gk.VARIABLE] = key
        return subprocess.run([sys.executable, "-I", script] + list(args), env=env, capture_output=True,
                              text=True, timeout=60)

    def test_found_and_not_found_without_showing_the_key(self):
        self.folder.file("logs/curia.log", "2026-10-20T19:00:00.000Z info ask_done gen=1\n")
        self.folder.file("crash/curia-2026.ips", b"{}\n" + base64.b64encode(b"x" + KEY.encode() + b"yz"))
        self.folder.file("old/" + "notes.txt", "token " + json.dumps(KEY))
        done = self.run_tool([self.folder.path], KEY)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn("KEY FOUND: crash/curia-2026.ips: base64 1", done.stdout)
        self.assertIn("KEY FOUND: old/notes.txt: raw (also JSON-escaped, URL-encoded) 1", done.stdout)
        self.assertIn("files scanned: 3", done.stdout)
        self.assert_nothing_secret(KEY, done.stdout, done.stderr)
        other = "sk-ant-FAKE-another-key-not-in-any-file-0000"
        done = self.run_tool([self.folder.path], other)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("Result: the key was found nowhere (3 file(s) scanned).", done.stdout)
        self.assertIn("shape sk-ant- with FAKE only (test keys): 1 file(s), 1 match(es)", done.stdout)
        self.assert_nothing_secret(other, done.stdout, done.stderr)
        self.assert_nothing_secret(KEY, done.stdout, done.stderr)

    def test_without_the_variable_or_with_the_key_as_an_argument(self):
        path = self.folder.file("a.log", "x")
        done = self.run_tool([path], None)
        self.assertEqual(done.returncode, 2)
        self.assertEqual(done.stdout, "")
        done = self.run_tool([path, KEY], KEY)
        self.assertEqual(done.returncode, 2)
        self.assertIn("typed on the command line", done.stderr)
        self.assert_nothing_secret(KEY, done.stdout, done.stderr)


class UsageTests(GrepKeyTestCase):
    def test_the_variable_must_be_set_and_long_enough(self):
        path = self.folder.file("a.log", "x")
        code, out, err = run_main([path], key=None)
        self.assertEqual(code, 2)
        self.assertIn("not set", err)
        code, out, err = run_main([path], key="FAKE123")
        self.assertEqual(code, 2)
        self.assertIn("shorter than 8", err)
        self.assertNotIn("FAKE123", out + err)
        code, _, _ = run_main([path], key="   FAKE   ")
        self.assertEqual(code, 2)

    def test_a_key_on_the_command_line_is_refused_without_echo(self):
        path = self.folder.file("a.log", "x")
        for args in ([path, KEY], [path, "--name-contains=" + KEY], ["--bogus", KEY]):
            with self.subTest(args=len(args)):
                code, out, err = run_main(args)
                self.assertEqual(code, 2)
                self.assertIn("typed on the command line", err)
                self.assert_nothing_secret(KEY, out, err)

    def test_paths_that_do_not_exist(self):
        missing = os.path.join(self.folder.path, "nope")
        code, out, err = run_main([missing])
        self.assertEqual(code, 2)
        self.assertIn("none of the paths exists", err)
        present = self.folder.file("a.log", "x")
        code, out, err = run_main([missing, present])
        self.assertEqual(code, 0)
        self.assertIn("does not exist (skipped)", out)
        self.assertIn("Result: the key was found nowhere (1 file(s) scanned). 1 path(s) do not exist (skipped).", out)

    def test_the_home_folder_is_printed_as_a_tilde(self):
        self.folder.file("logs/curia.log", "fine")
        logs = os.path.join(self.folder.path, "logs")
        code, out, err = run_main([logs, self.folder.path], env={"HOME": self.folder.path})
        self.assertEqual(code, 0, out + err)
        self.assertIn("Path 1: ~/logs\n", out)
        self.assertIn("Path 2: ~\n", out)
        self.assertNotIn(self.folder.path, out)
        self.assertEqual(gk.home_as_tilde("/home/someone-else/x", "/home/someone"), "/home/someone-else/x")
        self.assertEqual(gk.home_as_tilde("/x", "/"), "/x")

    def test_an_internal_error_prints_a_fixed_text(self):
        path = self.folder.file("a.log", "x")
        original = gk.run

        def broken(*args, **kwargs):
            raise ValueError("bytes near " + KEY)
        gk.run = broken
        self.addCleanup(setattr, gk, "run", original)
        code, out, err = run_main([path])
        self.assertEqual(code, 2)
        self.assertEqual(out, "")
        self.assertIn("internal error", err)
        self.assert_nothing_secret(KEY, out, err)

    def test_bad_options(self):
        path = self.folder.file("a.log", "x")
        self.assertEqual(run_main([path, "--max-bytes", "0"])[0], 2)
        self.assertEqual(run_main([path, "--max-bytes", "many"])[0], 2)
        self.assertEqual(run_main([])[0], 2)
        self.assertEqual(run_main(["--help"])[0], 0)

    def test_a_bad_argument_with_a_piece_of_the_key_is_not_repeated(self):
        # argparse repeats a bad value in its own message; the tool's usage errors are fixed texts.
        path = self.folder.file("a.log", "x")
        piece = KEY[:20]
        for args in ([path, "--max-bytes", piece], [path, "--" + piece], [path, "--name-contains"]):
            code, out, err = run_main(args)
            self.assertEqual(code, 2)
            self.assertNotIn(piece, out + err)
            self.assertIn("usage error", err)
            self.assert_nothing_secret(KEY, out, err)

    def test_a_size_cap_below_one_mib_is_shown_in_bytes(self):
        path = self.folder.file("big.log", "y" * 5000)
        code, out, _ = run_main([path, "--max-bytes", "4096"])
        self.assertEqual(code, 3)
        self.assertIn("only the first 4096 bytes", out)


if __name__ == "__main__":
    unittest.main()
