# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/release_blockers.sh. Each test builds a small fake tree in a temporary folder (the script
takes the root as its argument): every blocker present, none present, one at a time, and the exit codes of the warn
and --enforce modes. The last test runs the script on the real repository in warn mode."""
import json
import pathlib
import subprocess
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parent / "release_blockers.sh"
REPO = SCRIPT.parent.parent.parent

RELEASE_OFF = {"configurePresets": [{"name": "macos-release", "cacheVariables": {"CURIA_ENABLE_DEV_ENV": "OFF"}}]}
RELEASE_ON = {"configurePresets": [{"name": "macos-release", "cacheVariables": {"CURIA_ENABLE_DEV_ENV": "ON"}}]}
NO_RELEASE = {"configurePresets": [{"name": "macos-arm64", "cacheVariables": {"CURIA_ENABLE_DEV_ENV": "OFF"}}]}

# A tree with nothing to remove: the real mod's files without the probe parts, a release preset with the development
# configuration off.
CLEAN = {
    "app/CMakeLists.txt": "add_library(curia_core STATIC src/x.cpp)\nadd_executable(curia src/main.cpp)\n",
    "app/tests/test_cli.cpp": "// a test\n",
    "mod/curia/gui/curia_widget.gui": 'button = { name = "curia_advisor_button" }\n',
    "mod/curia/common/scripted_guis/curia_scripted_guis.txt": "curia_run_advisor = { }\n",
    "mod/curia/common/scripted_effects/curia_export.txt": "curia_export_run = { }\n",
    "mod/curia/localization/english/curia_l_english.yml": 'l_english:\n curia_button_advisor: "Advisor"\n',
    "CMakeLists.txt": "add_subdirectory(app)\n",
    "CMakePresets.json": json.dumps(RELEASE_OFF),
}

# Each blocker on its own: the files to add (or replace) and a fragment of the line that names it.
BLOCKERS = {
    "button": ({"mod/curia/gui/curia_widget.gui": 'button = { name = "curia_probes_button" }\n'}, "curia_probes_button"),
    "scripted gui": ({"mod/curia/common/scripted_guis/curia_scripted_guis.txt": "curia_run_probes = { }\n"},
                     "curia_run_probes"),
    "probe effects": ({"mod/curia/common/scripted_effects/curia_probes.txt": "# probes\n"}, "curia_probes.txt"),
    "localization": ({"mod/curia/localization/english/curia_l_english.yml":
                      'l_english:\n curia_probe_ruler_id: "x"\n'}, "curia_probe_* localization"),
    "probe mode": ({"mod/curia/common/script_values/curia_values.txt": "value = global_var:curia_probe_mode\n"},
                   "curia_probe_mode"),
    "m0 mod": ({"mod/curia-m0/descriptor.mod": 'name = "m0"\n'}, "mod/curia-m0"),
    "m2 mod": ({"mod/curia-m2-probe/descriptor.mod": 'name = "m2"\n'}, "mod/curia-m2-probe"),
    "app probe": ({"app/probe/probe_main.mm": "// probe\n"}, "app/probe/"),
    "probe tests": ({"app/tests/test_probe_combos.cpp": "// test\n"}, "test_probe_combos.cpp"),
    "m2 target": ({"app/CMakeLists.txt": CLEAN["app/CMakeLists.txt"] + "add_executable(curia_m2_probe p.mm)\n"},
                  "target curia_m2_probe"),
    "core target": ({"app/CMakeLists.txt": CLEAN["app/CMakeLists.txt"] + "add_library(curia_probe_core STATIC)\n"},
                    "target curia_probe_core"),
    "dev env on": ({"CMakePresets.json": json.dumps(RELEASE_ON)}, "CURIA_ENABLE_DEV_ENV off"),
    "no release preset": ({"CMakePresets.json": json.dumps(NO_RELEASE)}, "CURIA_ENABLE_DEV_ENV off"),
}


def run(files, *args):
    """Write `files` (relative path -> text) into a fresh temp tree and run the script on it."""
    with tempfile.TemporaryDirectory() as tmp:
        for rel, text in files.items():
            path = pathlib.Path(tmp) / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        return run_on(tmp, *args)


def run_on(root, *args):
    done = subprocess.run(["bash", str(SCRIPT), *args, str(root)], capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def listed(output):
    return [line for line in output.splitlines() if line.startswith("  - ")]


class ReleaseBlockers(unittest.TestCase):
    def test_clean_tree_has_none(self):
        for args in ([], ["--enforce"]):
            with self.subTest(args=args):
                code, out = run(CLEAN, *args)
                self.assertEqual(code, 0, out)
                self.assertIn("release blockers: none", out)
                self.assertEqual(listed(out), [])

    def test_each_blocker_alone_is_listed(self):
        for name, (files, fragment) in BLOCKERS.items():
            with self.subTest(blocker=name):
                tree = dict(CLEAN)
                tree.update(files)
                code, out = run(tree)
                self.assertEqual(code, 0, out)  # warn mode
                self.assertEqual(len(listed(out)), 1, out)
                self.assertIn(fragment, listed(out)[0])
                self.assertIn("warning only", out)
                code, out = run(tree, "--enforce")
                self.assertEqual(code, 1, out)
                self.assertNotIn("warning only", out)

    def test_everything_present(self):
        tree = dict(CLEAN)
        for name, (files, _) in BLOCKERS.items():
            if name in ("dev env on", "core target"):
                continue  # the same files as "no release preset" and "m2 target"
            tree.update(files)
        tree["app/CMakeLists.txt"] += "add_library(curia_probe_core STATIC)\n"
        code, out = run(tree)
        self.assertEqual(code, 0, out)
        self.assertEqual(len(listed(out)), 12, out)
        self.assertIn("12 still present", out)
        code, _ = run(tree, "--enforce")
        self.assertEqual(code, 1)

    def test_no_presets_file_counts_as_dev_env_on(self):
        tree = dict(CLEAN)
        del tree["CMakePresets.json"]
        code, out = run(tree)
        self.assertEqual(code, 0, out)
        self.assertEqual(len(listed(out)), 1, out)
        self.assertIn("CURIA_ENABLE_DEV_ENV off", out)

    def test_unreadable_presets_count_as_dev_env_on(self):
        tree = dict(CLEAN)
        tree["CMakePresets.json"] = "{ not json"
        code, out = run(tree, "--enforce")
        self.assertEqual(code, 1, out)
        self.assertIn("CURIA_ENABLE_DEV_ENV off", out)

    def test_build_trees_and_hidden_folders_are_not_read(self):
        tree = dict(CLEAN)
        tree["build/x/CMakeLists.txt"] = "add_executable(curia_m2_probe p.mm)\n"
        tree[".cache/w/app/CMakeLists.txt"] = "add_library(curia_probe_core STATIC)\n"
        code, out = run(tree, "--enforce")
        self.assertEqual(code, 0, out)

    def test_usage_errors(self):
        code, out = run(CLEAN, "--nope")
        self.assertEqual(code, 2, out)
        with tempfile.TemporaryDirectory() as tmp:
            code, out = run_on(tmp)
            self.assertEqual(code, 2, out)
            self.assertIn("no app/ and mod/", out)
        code, out = run_on("/nonexistent/curia-release-blockers")
        self.assertEqual(code, 2, out)

    def test_real_repository_warns_without_failing(self):
        code, out = run_on(REPO)
        self.assertEqual(code, 0, out)
        self.assertTrue(out.startswith("release blockers:"), out)


if __name__ == "__main__":
    unittest.main()
