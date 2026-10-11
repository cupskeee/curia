# SPDX-License-Identifier: MIT
"""Unit tests of tools/ci/lint_mod.py. Synthetic mini mods in temporary folders (no game files): one good case, then one
failing case per check; the last tests run the lint on the real mod/curia folder."""
import pathlib
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import lint_mod  # noqa: E402

SNAP = "[EmptyScope.ScriptValue('curia_snap_id_value')|0]"
NAME = "[THIS.GetCharacter.GetNameNoTooltip]"
ID = "[THIS.GetCharacter.GetID]"


def i(name):
    return "[EmptyScope.ScriptValue('%s')|0]" % name


BEGIN = "CURIA1|%s|BEGIN|%s|{version}|%s|%s|%s|%s|%s" % (
    SNAP, i("curia_mode_value"), i("curia_date_year"), i("curia_date_month"), i("curia_date_day"), ID, NAME)
FIN = "CURIA1|%s|FIN|[THIS.GetCharacter.GetGold|0]|[THIS.ScriptValue('curia_income_tenths')|0]" % SNAP

DESCRIPTOR = 'version="{version}"\ntags={{\n\t"Utilities"\n}}\nname="Curia"\nsupported_version="1.20.*"\n'
ON_ACTION = (
    "on_game_start_after_lobby = {\n\ton_actions = { curia_on_game_start }\n}\n"
    'curia_on_game_start = {\n\teffect = {\n\t\tdebug_log = "CURIA1|0|LOAD|{version}"\n\t}\n}\n'
)
EFFECTS = """# comment lines between statements are skipped
curia_export_begin = {
	set_global_variable = { name = curia_snap_id value = 1 }
	set_global_variable = { name = curia_export_lines value = 1 }
	debug_log = curia_line_begin
}

curia_export_fin = {
	# the increment is directly before the debug_log
	change_global_variable = { name = curia_export_lines add = 1 }
	debug_log = curia_line_fin
}
"""
VALUES = "".join(
    "%s = {\n\tvalue = %s\n}\n" % (name, body)
    for name, body in [
        ("curia_snap_id_value", "global_var:curia_snap_id"),
        ("curia_line_count_value", "global_var:curia_export_lines"),
        ("curia_mode_value", "0"),
        ("curia_date_year", "current_year"),
        ("curia_date_month", "current_month"),
        ("curia_date_day", "current_day"),
        ("curia_income_tenths", "monthly_character_income"),
    ]
)


def loc(**keys):
    return "l_english:\n" + "".join(' %s: "%s"\n' % (k, v) for k, v in keys.items())


def good_files(version="0.2.0"):
    return {
        "descriptor.mod": DESCRIPTOR.format(version=version),
        "common/on_action/curia_on_actions.txt": ON_ACTION.replace("{version}", version),
        "common/scripted_effects/curia_export.txt": EFFECTS,
        "common/script_values/curia_values.txt": VALUES,
        "localization/english/curia_l_english.yml": loc(
            curia_line_begin=BEGIN.format(version=version), curia_line_fin=FIN),
    }


def write_mod(tmp, files, name="curia"):
    """Write a mini mod below tmp/mod/<name>; .txt and .yml files get a UTF-8 BOM like the real ones."""
    mod = pathlib.Path(tmp) / "mod" / name
    for rel, text in files.items():
        path = mod / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        data = text.encode("utf-8")
        if path.suffix in (".txt", ".yml"):
            data = lint_mod.BOM + data
        path.write_bytes(data)
    return mod


class LintCase(unittest.TestCase):
    def mod_with(self, changes=None):
        """Return a mini mod; `changes` maps a file to new text, or to an (old, new) replacement pair."""
        files = good_files()
        for rel, change in (changes or {}).items():
            if isinstance(change, tuple):
                old, new = change
                self.assertIn(old, files[rel], f"test setup: {old!r} not in {rel}")
                files[rel] = files[rel].replace(old, new)
            else:
                files[rel] = change
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        return write_mod(tmp.name, files)

    def assertProblem(self, problems, *needles):
        text = "\n".join(problems)
        self.assertTrue(problems, "expected a problem, the lint found none")
        for needle in needles:
            self.assertIn(needle, text)


EFFECTS_FILE = "common/scripted_effects/curia_export.txt"
LOC_FILE = "localization/english/curia_l_english.yml"
VALUES_FILE = "common/script_values/curia_values.txt"
ON_ACTION_FILE = "common/on_action/curia_on_actions.txt"


class GoodCase(LintCase):
    def test_good_mini_mod_passes_every_check(self):
        mod = self.mod_with()
        self.assertEqual(lint_mod.lint_structure(mod, mod.parent), [])
        self.assertEqual(lint_mod.lint_export(mod, coverage=False), [])

    def test_lint_all_counts_mod_folders(self):
        mod = self.mod_with()
        problems, count = lint_mod.lint_all(mod.parent)
        self.assertEqual(count, 1)
        # the mini mod covers only two record forms, so the coverage check is the one that speaks
        # (and the wiring check, which needs the sections of a complete mod)
        self.assertTrue(all("no curia_line_ localization key" in p or "scripted effect" in p or "counting global" in p
                            or "is not defined" in p for p in problems), problems)


class VersionAgreement(LintCase):
    def test_descriptor_differs_from_load_literal(self):
        mod = self.mod_with({"descriptor.mod": ('version="0.2.0"', 'version="0.3.0"')})
        self.assertProblem(lint_mod.check_version_agreement(mod), "LOAD literal version '0.2.0'", "0.3.0")

    def test_begin_localization_differs(self):
        mod = self.mod_with({LOC_FILE: ("|0.2.0|", "|0.9.0|")})
        self.assertProblem(lint_mod.check_version_agreement(mod), "curia_line_begin", "0.9.0")

    def test_metadata_differs(self):
        files = good_files()
        files[".metadata/metadata.json"] = '{"name": "Curia", "version": "0.1.0"}\n'
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod = write_mod(tmp.name, files)
        self.assertProblem(lint_mod.check_version_agreement(mod), "metadata.json", "0.1.0")

    def test_load_literal_missing(self):
        mod = self.mod_with({ON_ACTION_FILE: "curia_on_game_start = {\n}\n"})
        self.assertProblem(lint_mod.check_version_agreement(mod), "no LOAD literal")


class DebugLogKeys(LintCase):
    def test_undefined_key(self):
        mod = self.mod_with({EFFECTS_FILE: ("debug_log = curia_line_fin", "debug_log = curia_line_finn")})
        self.assertProblem(lint_mod.check_debug_log_keys(mod), "curia_line_finn", "not defined")

    def test_unused_key(self):
        extra = ' curia_line_heir_none: "CURIA1|%s|HEIR|none"\n' % SNAP
        mod = self.mod_with({LOC_FILE: ("\n curia_line_fin:", "\n" + extra + " curia_line_fin:")})
        problems = lint_mod.check_debug_log_keys(mod)
        self.assertProblem(problems, "curia_line_heir_none", "no debug_log uses it")


class DebugLogCounter(LintCase):
    def test_missing_increment(self):
        mod = self.mod_with({EFFECTS_FILE: (
            "	change_global_variable = { name = curia_export_lines add = 1 }\n	debug_log = curia_line_fin", "	debug_log = curia_line_fin")})
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "curia_export.txt", "debug_log = curia_line_fin")

    def test_another_statement_between_increment_and_log(self):
        mod = self.mod_with({EFFECTS_FILE: (
            "add = 1 }\n	debug_log = curia_line_fin", "add = 1 }\n	set_global_variable = { name = curia_snap_id value = 2 }\n	debug_log = curia_line_fin")})
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "curia_line_fin")

    def test_begin_needs_the_set_to_one(self):
        mod = self.mod_with({EFFECTS_FILE: (
            "	set_global_variable = { name = curia_export_lines value = 1 }\n	debug_log = curia_line_begin",
            "	change_global_variable = { name = curia_export_lines add = 1 }\n	debug_log = curia_line_begin")})
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "curia_line_begin", "value = 1")

    def test_inline_literal_in_effect(self):
        mod = self.mod_with({EFFECTS_FILE: ("debug_log = curia_line_fin", 'debug_log = "CURIA1|1|FIN|1|2"')})
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "inline literal")

    def test_other_literal_in_on_action(self):
        mod = self.mod_with({ON_ACTION_FILE: ('LOAD|0.2.0', 'HELLO|0.2.0')})
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "not the LOAD literal")


class Grammar(LintCase):
    def with_fin(self, value):
        return self.mod_with({LOC_FILE: (FIN, value)})

    def grammar(self, mod):
        return lint_mod.check_localization_grammar(mod)[0]

    def test_missing_prefix(self):
        self.assertProblem(self.grammar(self.with_fin("FIN|1|2")), "curia_line_fin", "must start with")

    def test_wrong_snap_expression(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace(SNAP, i("curia_mode_value")))), "must start with")

    def test_too_few_fields(self):
        self.assertProblem(self.grammar(self.with_fin("CURIA1|%s|FIN|[THIS.GetCharacter.GetGold|0]" % SNAP)),
                           "curia_line_fin", "has 2 fields")

    def test_too_many_fields(self):
        self.assertProblem(self.grammar(self.with_fin(FIN + "|" + i("curia_mode_value"))), "has 2 fields", "found 3")

    def test_unknown_record_type(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("|FIN|", "|FOO|"))), "FOO", "not in the table")

    def test_lowercase_record_type(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("|FIN|", "|fin|"))), "does not match")

    def test_text_expression_not_last(self):
        mod = self.mod_with({LOC_FILE: (ID + "|" + NAME, NAME + "|" + ID)})
        self.assertProblem(self.grammar(mod), "curia_line_begin", "must be the last field")

    def test_two_text_expressions(self):
        mod = self.mod_with({LOC_FILE: (ID + "|" + NAME, NAME + "|" + NAME)})
        self.assertProblem(self.grammar(mod), "more than one text expression")

    def test_text_expression_mixed_with_literal(self):
        mod = self.mod_with({LOC_FILE: ("|" + NAME, "|Ruler " + NAME)})
        self.assertProblem(self.grammar(mod), "mixed with literal text")

    def test_pipe_inside_expression(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("GetGold|0", "GetGold|0|1"))), "pipe inside an expression")

    def test_markup_inside_expression(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("GetGold|0", "GetGold|0#bold"))), "markup")

    def test_unproven_integer_form(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("GetGold|0", "GetGold|2"))), "proven integer expression")

    def test_unproven_text_form(self):
        mod = self.mod_with({LOC_FILE: ("|" + NAME, "|[THIS.GetCharacter.GetFullName]")})
        self.assertProblem(self.grammar(mod), "proven text expression")

    def test_undefined_script_value(self):
        self.assertProblem(self.grammar(self.with_fin(FIN.replace("curia_income_tenths", "curia_income_nope"))),
                           "curia_income_nope", "does not define")

    def test_enum_is_case_sensitive(self):
        files = good_files()
        files["localization/english/curia_l_english.yml"] += ' curia_line_claim_x: "CURIA1|%s|CLAIM|%s|Weak|%s|[THIS.Title.GetNameNoTierNoTooltip]"\n' % (
            SNAP, i("curia_mode_value"), i("curia_mode_value"))
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod = write_mod(tmp.name, files)
        self.assertProblem(self.grammar(mod), "curia_line_claim_x", "expected the literal")

    def test_none_form_takes_nothing_after_it(self):
        files = good_files()
        files["localization/english/curia_l_english.yml"] += ' curia_line_heir_none: "CURIA1|%s|HEIR|none|"\n' % SNAP
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod = write_mod(tmp.name, files)
        self.assertProblem(self.grammar(mod), "curia_line_heir_none")

    def test_probe_key_needs_the_prefix_but_not_the_table(self):
        files = good_files()
        loc_path = "localization/english/curia_l_english.yml"
        files[loc_path] += ' curia_probe_a: "CURIA1|%s|PROBE|anything|[THIS.GetCharacter.GetFirstNameNoTooltip]|1"\n' % SNAP
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.assertEqual(self.grammar(write_mod(tmp.name, files)), [])
        files[loc_path] += ' curia_probe_b: "PROBE|x"\n'
        tmp2 = tempfile.TemporaryDirectory()
        self.addCleanup(tmp2.cleanup)
        self.assertProblem(self.grammar(write_mod(tmp2.name, files)), "curia_probe_b", "must start with")

    def test_probe_key_must_be_a_probe_record(self):
        files = good_files()
        files["localization/english/curia_l_english.yml"] += ' curia_probe_c: "%s"\n' % FIN
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.assertProblem(self.grammar(write_mod(tmp.name, files)), "curia_probe_c", "must be a PROBE record")

    def test_coverage_names_the_missing_form(self):
        mod = self.mod_with()
        self.assertProblem(lint_mod.check_table_coverage(mod), "'HEIR none'", "'FACTION TARGET'")
        self.assertNotIn("'FIN'", "\n".join(lint_mod.check_table_coverage(mod)))

    def test_split_fields(self):
        self.assertEqual(lint_mod.split_fields("a|[x|0]|b"), ["a", "[x|0]", "b"])
        with self.assertRaises(ValueError):
            lint_mod.split_fields("a|[x")


class Globals(LintCase):
    def test_set_but_never_read(self):
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_fin = {", "curia_export_fin = {\n	set_global_variable = { name = curia_orphan value = 1 }")})
        self.assertProblem(lint_mod.check_globals_read(mod), "curia_orphan", "curia_export.txt")

    def test_read_in_an_effect_does_not_count(self):
        # exists = global_var:x in a trigger does not make the variable "read" for the engine's check
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_fin = {", "curia_export_fin = {\n	set_global_variable = { name = curia_orphan value = 1 }\n	if = { limit = { global_var:curia_orphan = 1 } }")})
        self.assertProblem(lint_mod.check_globals_read(mod), "curia_orphan")

    def test_counter_not_reset_in_begin(self):
        files = {
            EFFECTS_FILE: ("curia_export_fin = {", "curia_export_fin = {\n	change_global_variable = { name = curia_rank add = 1 }"),
            VALUES_FILE: ("curia_snap_id_value", "curia_rank_value = {\n	value = global_var:curia_rank\n}\ncuria_snap_id_value"),
        }
        mod = self.mod_with(files)
        self.assertEqual(lint_mod.check_globals_read(mod), [])
        self.assertProblem(lint_mod.check_counters_reset(mod), "curia_rank", "curia_export_begin")

    def test_begin_effect_missing(self):
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_begin", "curia_export_start")})
        self.assertProblem(lint_mod.check_counters_reset(mod), "curia_export_begin is not defined")


class StructureCase(LintCase):
    def test_missing_bom_and_prefix(self):
        files = good_files()
        files["common/scripted_effects/other.txt"] = "x = {}\n"
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod = write_mod(tmp.name, files)
        (mod / "common/scripted_effects/other.txt").write_bytes(b"x = {}\n")
        self.assertProblem(lint_mod.lint_structure(mod, mod.parent), "other.txt: missing UTF-8 BOM", "must start with curia_")


class DebugLogPairs(LintCase):
    def pairs(self, mod):
        return lint_mod.check_increment_log_pairs(mod)

    def test_doubled_increment(self):
        mod = self.mod_with({EFFECTS_FILE: (
            "	change_global_variable = { name = curia_export_lines add = 1 }\n	debug_log = curia_line_fin",
            "	change_global_variable = { name = curia_export_lines add = 1 }\n"
            "	change_global_variable = { name = curia_export_lines add = 1 }\n	debug_log = curia_line_fin")})
        self.assertProblem(self.pairs(mod), "curia_export_fin", "2 write(s)", "1 debug_log")

    def test_orphan_increment(self):
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_fin = {", "curia_export_fin = {\n	change_global_variable = { name = curia_export_lines add = 1 }\n	change_global_variable = { name = curia_export_lines add = 1 }\n	debug_log = curia_line_fin\n}\ncuria_orphan = {\n	change_global_variable = { name = curia_export_lines add = 1 }")})
        self.assertProblem(self.pairs(mod), "curia_orphan")

    def test_debug_log_outside_scripted_effects(self):
        gui = "curia_run = {\n	scope = character\n	effect = {\n		debug_log = curia_line_fin\n	}\n}\n"
        mod = self.mod_with({"common/scripted_guis/curia_scripted_guis.txt": gui})
        self.assertProblem(self.pairs(mod), "curia_scripted_guis.txt", "curia_run", "0 write(s)")
        self.assertProblem(lint_mod.check_debug_log_counter(mod), "curia_scripted_guis.txt", "debug_log = curia_line_fin")

    def test_multi_line_increment_counts(self):
        mod = self.mod_with({EFFECTS_FILE: (
            "change_global_variable = { name = curia_export_lines add = 1 }",
            "change_global_variable = {\n		name = curia_export_lines\n		add = 1\n	}")})
        self.assertEqual(self.pairs(mod), [])


class MultiLineGlobals(LintCase):
    def test_multi_line_write_is_seen(self):
        write = "	set_global_variable = {\n		name = curia_zzz\n		value = 0\n	}\n"
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_fin = {\n", "curia_export_fin = {\n" + write)})
        self.assertProblem(lint_mod.check_globals_read(mod), "curia_zzz", "curia_export.txt")

    def test_multi_line_counter_not_reset(self):
        change = "	change_global_variable = {\n		name = curia_rank\n		add = 1\n	}\n"
        mod = self.mod_with({EFFECTS_FILE: ("curia_export_fin = {\n", "curia_export_fin = {\n" + change)})
        self.assertProblem(lint_mod.check_counters_reset(mod), "curia_rank", "curia_export_begin")


class Indentation(LintCase):
    def test_tab_indent_in_localization(self):
        mod = self.mod_with({LOC_FILE: ("\n curia_line_fin:", "\n\tcuria_line_fin:")})
        self.assertProblem(lint_mod.lint_structure(mod, mod.parent), "exactly one space")

    def test_two_space_indent_in_localization(self):
        mod = self.mod_with({LOC_FILE: ("\n curia_line_fin:", "\n  curia_line_fin:")})
        self.assertProblem(lint_mod.lint_structure(mod, mod.parent), "exactly one space")


class ClosestForm(LintCase):
    def test_heir_with_a_missing_field_names_the_heir_form(self):
        fields = lint_mod.split_fields("CURIA1|%s|HEIR|%s|%s|%s|%s" % (SNAP, ID, i("curia_has_liege_value"), i("curia_is_landed_value"), NAME))[3:]
        form, reason = lint_mod.match_record("HEIR", fields)
        self.assertIsNone(form)
        self.assertIn("form 'HEIR' has 5 fields", reason)

    def test_war_attacker_with_a_missing_field_names_the_attacker_form(self):
        form, reason = lint_mod.match_record("WAR", [i("curia_war_index_value"), "ATTACKER", NAME])
        self.assertIsNone(form)
        self.assertIn("form 'WAR ATTACKER'", reason)

    def test_faction_target_with_a_missing_field_names_the_target_form(self):
        form, reason = lint_mod.match_record("FACTION", [i("curia_faction_index_value"), "TARGET", NAME])
        self.assertIsNone(form)
        self.assertIn("form 'FACTION TARGET'", reason)


class RealModMutants(unittest.TestCase):
    """Copy the real mod, break one thing, and expect the lint to say so (the mutants the reviewers tried)."""

    def mutate(self, rel, old, new, count=1):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        mod = pathlib.Path(tmp.name) / "mod" / "curia"
        shutil.copytree(lint_mod.REPO_ROOT / "mod" / "curia", mod)
        path = mod / rel
        data = path.read_bytes().decode("utf-8")
        self.assertIn(old, data, f"test setup: {old!r} not in {rel}")
        path.write_bytes(data.replace(old, new, count).encode("utf-8"))
        return mod

    def problems(self, mod):
        return lint_mod.lint_all(mod.parent)[0]

    def assertCaught(self, mod, *needles):
        text = "\n".join(self.problems(mod))
        self.assertTrue(text, "the lint found nothing")
        for needle in needles:
            self.assertIn(needle, text)

    def test_claim_n_from_the_wrong_counter(self):
        mod = self.mutate(LOC_FILE, "CLAIM|[EmptyScope.ScriptValue('curia_claim_index_value')|0]|strong",
                          "CLAIM|[EmptyScope.ScriptValue('curia_war_index_value')|0]|strong")
        self.assertCaught(mod, "curia_line_claim_strong", "field 1 must read curia_claim_index_value")

    def test_vassal_id_and_gold_swapped(self):
        mod = self.mutate(LOC_FILE, "|[THIS.GetCharacter.GetID]|[THIS.GetCharacter.GetGold|0]|[THIS.ScriptValue('curia_opinion_of_liege')",
                          "|[THIS.GetCharacter.GetGold|0]|[THIS.GetCharacter.GetID]|[THIS.ScriptValue('curia_opinion_of_liege')")
        self.assertCaught(mod, "curia_line_vassal", "field 2 must read")

    def test_wars_header_reads_the_wrong_value(self):
        mod = self.mutate(LOC_FILE, "WARS|[EmptyScope.ScriptValue('curia_wars_total_value')|0]",
                          "WARS|[EmptyScope.ScriptValue('curia_line_count_value')|0]")
        self.assertCaught(mod, "curia_line_wars", "must read curia_wars_total_value")

    def test_vassals_header_reads_the_computed_count(self):
        mod = self.mutate(LOC_FILE, "|[EmptyScope.ScriptValue('curia_vassals_listed_value')|0]\"",
                          "|[THIS.ScriptValue('curia_vassal_count')|0]\"")
        self.assertCaught(mod, "curia_line_vassals", "must read curia_vassals_listed_value")

    def test_counting_pass_increment_removed(self):
        mod = self.mutate(EFFECTS_FILE, "			change_global_variable = { name = curia_vassals_listed add = 1 }\n", "")
        self.assertCaught(mod, "curia_vassals_listed", "never changed")

    def test_section_removed_from_the_run(self):
        mod = self.mutate(EFFECTS_FILE, "	curia_export_claims = yes\n", "")
        self.assertCaught(mod, "curia_export_run calls", "order of docs/wire-format.md")

    def test_sections_swapped(self):
        mod = self.mutate(EFFECTS_FILE, "	curia_export_wars = yes\n	curia_export_claims = yes\n",
                          "	curia_export_claims = yes\n	curia_export_wars = yes\n")
        self.assertCaught(mod, "curia_export_run calls", "order of docs/wire-format.md")

    def test_probes_after_end_are_rejected(self):
        mod = self.mutate(EFFECTS_FILE, "	curia_export_end = yes\n}", "	curia_export_end = yes\n	curia_export_probes = yes\n}")
        self.assertCaught(mod, "curia_export_run calls")

    def test_iterator_max_differs_between_passes(self):
        mod = self.mutate(EFFECTS_FILE, "max = curia_max_vassals", "max = curia_max_factions")
        self.assertCaught(mod, "curia_export_vassals", "different parameters")

    def test_iterator_order_differs_between_passes(self):
        mod = self.mutate(EFFECTS_FILE, "order_by = tier", "order_by = tier\n			pressed = yes")
        self.assertCaught(mod, "curia_export_claims", "different parameters")

    def test_second_pass_missing(self):
        mod = self.mutate(EFFECTS_FILE, "ordered_targeting_faction = {", "every_targeting_faction = {", count=1)
        self.assertCaught(mod, "curia_export_factions", "twice")

    def test_double_increment_in_the_real_mod(self):
        line = "change_global_variable = { name = curia_export_lines add = 1 }"
        mod = self.mutate(EFFECTS_FILE, "	" + line + "\n	debug_log = curia_line_fin", "	" + line + "\n	" + line + "\n	debug_log = curia_line_fin")
        self.assertCaught(mod, "curia_export_fin", "2 write(s)")

    def test_debug_log_without_increment_in_the_scripted_gui(self):
        mod = self.mutate("common/scripted_guis/curia_scripted_guis.txt", "curia_export_run = yes",
                          "debug_log = curia_line_fin\n		curia_export_run = yes")
        self.assertCaught(mod, "curia_scripted_guis.txt", "curia_line_fin")

    def test_multi_line_orphan_global(self):
        mod = self.mutate(EFFECTS_FILE, "curia_export_fin = {\n",
                          "curia_export_fin = {\n	set_global_variable = {\n		name = curia_zzz\n		value = 0\n	}\n")
        self.assertCaught(mod, "curia_zzz")

    def test_tab_indent_in_the_real_localization(self):
        mod = self.mutate(LOC_FILE, "\n curia_line_fin:", "\n\tcuria_line_fin:")
        self.assertCaught(mod, "exactly one space")


class RealMod(unittest.TestCase):
    def test_real_mod_passes_everything_including_coverage(self):
        problems, count = lint_mod.lint_all(lint_mod.REPO_ROOT / "mod")
        self.assertEqual(problems, [])
        self.assertGreaterEqual(count, 1)

    def test_real_mod_covers_every_record_form(self):
        mod = lint_mod.REPO_ROOT / "mod" / "curia"
        self.assertEqual(lint_mod.check_table_coverage(mod), [])

    def test_command_line_output(self):
        result = subprocess.run([sys.executable, str(pathlib.Path(lint_mod.__file__))], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertRegex(result.stdout, r"^mod lint ok \(\d+ mod folder\(s\)\)$")


if __name__ == "__main__":
    unittest.main()
