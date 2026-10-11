#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Structural and format lint of the CK3 mod folders. Uses no game files (CI never touches Paradox content).

Structure, for every folder under mod/ that has a descriptor.mod:
  * descriptor.mod exists, has no BOM, and defines name, version and supported_version
  * script (.txt) and localization (.yml) files start with a UTF-8 BOM, like the game's own files
  * localization files start with "l_english:" (after the BOM) and use no tabs for indentation
  * no CRLF line endings (we keep LF)
  * file names carry the curia_ prefix (to avoid colliding with other mods)

Export format, for the mod folder named "curia" only (docs/wire-format.md; the checks make its invariants machine-checked):
  1. version agreement: descriptor.mod, the on_action LOAD literal and the BEGIN localization line (and metadata.json)
  2. every debug_log names a localization key; every curia_line_* / curia_probe_* key is used
  3. every debug_log in a scripted effect directly follows the curia_export_lines increment (BEGIN: the set to 1), and
     no inline literal other than the LOAD literal exists
  4. localization grammar: record prefix, field count and kinds from an executable table, one text field and only last,
     and for the counters and header totals the script value each field must read
  5. every global variable the mod sets is read as global_var:<name> in a script value, and every counter is reset in
     curia_export_begin (multi-line writes included)
  6. wiring: per effect the number of curia_export_lines writes equals the number of debug_log calls; curia_export_run
     calls the sections in the order of docs/wire-format.md section 4; the counting and writing pass of a section have
     identical iterator parameters; every counting global is changed somewhere
The real validation against the game is local only (ck3-tiger, docs/architecture.md section 7).
"""
import json
import pathlib
import re
import sys

BOM = b"\xef\xbb\xbf"
REPO_ROOT = pathlib.Path(__file__).resolve().parents[2]
CURIA_MOD = "curia"

# ---------------------------------------------------------------------------------------------------------------------
# The record table (docs/wire-format.md section 4) as data. A field kind is one of:
#   ("int",)       one of the proven integer expressions (INT_FORMS)
#   ("lit", word)  the literal enum word or sub-kind
#   ("token",)     a literal version token
#   ("text",)      one of the proven text expressions (TEXT_FORMS); only ever the last field
#   ("none",)      the literal word none, with nothing after it
# Each form is (form id, record type, field kinds after the type). A record type with several forms (the none form, the
# enum values) has one form per variant; every variant needs its own localization key, because a debug_log prints one key.
# ---------------------------------------------------------------------------------------------------------------------
INT = ("int",)
TOKEN = ("token",)
TEXT = ("text",)
NONE = ("none",)


def LIT(word):
    return ("lit", word)


FORMS = [
    ("BEGIN", "BEGIN", [INT, TOKEN, INT, INT, INT, INT, TEXT]),
    ("FIN", "FIN", [INT, INT]),
    ("HEIR none", "HEIR", [NONE]),
    ("HEIR", "HEIR", [INT, INT, INT, INT, TEXT]),
    ("VASSALS", "VASSALS", [INT, INT]),
    ("VASSAL", "VASSAL", [INT, INT, INT, INT, INT, TEXT]),
    ("WARS", "WARS", [INT]),
    ("WAR NAME", "WAR", [INT, LIT("NAME"), TEXT]),
    ("WAR ATTACKER", "WAR", [INT, LIT("ATTACKER"), INT, TEXT]),
    ("WAR DEFENDER", "WAR", [INT, LIT("DEFENDER"), INT, TEXT]),
    ("CLAIMS", "CLAIMS", [INT, INT, INT]),
    ("CLAIM strong", "CLAIM", [INT, LIT("strong"), INT, TEXT]),
    ("CLAIM weak", "CLAIM", [INT, LIT("weak"), INT, TEXT]),
    ("FACTIONS", "FACTIONS", [INT, INT, INT]),
    ("FACTION INFO against", "FACTION", [INT, LIT("INFO"), LIT("against"), INT, INT, INT, INT]),
    ("FACTION INFO member", "FACTION", [INT, LIT("INFO"), LIT("member"), INT, INT, INT, INT]),
    ("FACTION LEADER none", "FACTION", [INT, LIT("LEADER"), NONE]),
    ("FACTION LEADER", "FACTION", [INT, LIT("LEADER"), INT, TEXT]),
    ("FACTION CLAIMANT none", "FACTION", [INT, LIT("CLAIMANT"), NONE]),
    ("FACTION CLAIMANT", "FACTION", [INT, LIT("CLAIMANT"), INT, TEXT]),
    ("FACTION TITLE none", "FACTION", [INT, LIT("TITLE"), NONE]),
    ("FACTION TITLE", "FACTION", [INT, LIT("TITLE"), INT, TEXT]),
    ("FACTION TARGET none", "FACTION", [INT, LIT("TARGET"), NONE]),
    ("FACTION TARGET", "FACTION", [INT, LIT("TARGET"), INT, TEXT]),
    ("END", "END", [INT]),
]

# Which source a field must read, so that a swapped counter or header total cannot pass the grammar alone. Key: (form id,
# field number after the type, counted from 1). A value that starts with "[" is the exact expression, any other value is
# the name of the script value.
_ID = "[THIS.GetCharacter.GetID]"
_GOLD = "[THIS.GetCharacter.GetGold|0]"
EXPECTED_SOURCE = {
    ("BEGIN", 1): "curia_mode_value",
    ("BEGIN", 3): "curia_date_year",
    ("BEGIN", 4): "curia_date_month",
    ("BEGIN", 5): "curia_date_day",
    ("BEGIN", 6): _ID,
    ("FIN", 1): _GOLD,
    ("FIN", 2): "curia_income_tenths",
    ("HEIR", 1): _ID,
    ("VASSALS", 1): "curia_vassal_count",
    ("VASSALS", 2): "curia_vassals_listed_value",
    ("VASSAL", 1): "curia_rank_value",
    ("VASSAL", 2): _ID,
    ("VASSAL", 3): _GOLD,
    ("VASSAL", 4): "curia_opinion_of_liege",
    ("VASSAL", 5): "curia_military_strength",
    ("WARS", 1): "curia_wars_total_value",
    ("WAR NAME", 1): "curia_war_index_value",
    ("WAR ATTACKER", 1): "curia_war_index_value",
    ("WAR ATTACKER", 3): _ID,
    ("WAR DEFENDER", 1): "curia_war_index_value",
    ("WAR DEFENDER", 3): _ID,
    ("CLAIMS", 1): "curia_claims_explicit_count",
    ("CLAIMS", 2): "curia_claims_implicit_count",
    ("CLAIMS", 3): "curia_claims_listed_value",
    ("CLAIM strong", 1): "curia_claim_index_value",
    ("CLAIM strong", 3): "curia_title_tier",
    ("CLAIM weak", 1): "curia_claim_index_value",
    ("CLAIM weak", 3): "curia_title_tier",
    ("FACTIONS", 1): "curia_faction_against_count",
    ("FACTIONS", 2): "curia_faction_member_flag",
    ("FACTIONS", 3): "curia_factions_listed_value",
    ("FACTION INFO against", 4): "curia_faction_type_code",
    ("FACTION INFO against", 5): "curia_faction_power",
    ("FACTION INFO against", 6): "curia_faction_power_threshold",
    ("FACTION INFO against", 7): "curia_faction_member_count",
    ("FACTION INFO member", 4): "curia_faction_type_code",
    ("FACTION INFO member", 5): "curia_faction_power",
    ("FACTION INFO member", 6): "curia_faction_power_threshold",
    ("FACTION INFO member", 7): "curia_faction_member_count",
    ("FACTION LEADER", 3): _ID,
    ("FACTION CLAIMANT", 3): _ID,
    ("FACTION TITLE", 3): "curia_title_tier",
    ("FACTION TARGET", 3): _ID,
    ("END", 1): "curia_line_count_value",
}
for _form in ("INFO against", "INFO member", "LEADER none", "LEADER", "CLAIMANT none", "CLAIMANT", "TITLE none", "TITLE",
              "TARGET none", "TARGET"):
    EXPECTED_SOURCE[("FACTION " + _form, 1)] = "curia_faction_index_value"

SNAP_EXPR = "[EmptyScope.ScriptValue('curia_snap_id_value')|0]"

# Integer expressions that printed plain digits in the game (M0/M0b/M3). Group 1, where present, is a script value name.
INT_FORMS = [
    re.compile(r"^\[EmptyScope\.ScriptValue\('(curia_[a-z0-9_]+)'\)\|0\]$"),
    re.compile(r"^\[THIS\.ScriptValue\('(curia_[a-z0-9_]+)'\)\|0\]$"),
    re.compile(r"^\[THIS\.GetCharacter\.GetGold\|0\]$"),
    re.compile(r"^\[THIS\.GetCharacter\.GetID\]$"),
]
# Text expressions that printed clean names in the game (no link markup): the last field of a record.
TEXT_FORMS = {
    "[THIS.GetCharacter.GetNameNoTooltip]",
    "[THIS.GetCharacter.GetPrimaryTitle.GetNameNoTierNoTooltip]",
    "[THIS.War.GetName]",
    "[THIS.Title.GetNameNoTierNoTooltip]",
}
TOKEN_RE = re.compile(r"^[A-Za-z0-9._+-]{1,32}$")
TYPE_RE = re.compile(r"^[A-Z][A-Z0-9_]*$")
LOAD_RE = re.compile(r"^CURIA1\|0\|LOAD\|([A-Za-z0-9._+-]{1,32})$")
MARKUP_CHARS = set('#$@\\"')

INC_LINE = "change_global_variable = { name = curia_export_lines add = 1 }"
SET1_LINE = "set_global_variable = { name = curia_export_lines value = 1 }"
BEGIN_EFFECT = "curia_export_begin"
BEGIN_KEY = "curia_line_begin"


# ---------------------------------------------------------------------------------------------------------------------
# Reading the files
# ---------------------------------------------------------------------------------------------------------------------
def read_text(path):
    return path.read_bytes().decode("utf-8-sig", errors="replace")


def mod_txt(mod, subdir):
    d = mod / "common" / subdir
    return sorted(d.glob("*.txt")) if d.is_dir() else []


def all_script_files(mod):
    d = mod / "common"
    return sorted(d.rglob("*.txt")) if d.is_dir() else []


def loc_files(mod):
    d = mod / "localization"
    return sorted(d.rglob("*.yml")) if d.is_dir() else []


def significant(text):
    """Yield (line number, normalised line) for lines that are not blank or comment-only."""
    for number, raw in enumerate(text.splitlines(), 1):
        line = re.sub(r"\s+", " ", raw.split("#", 1)[0]).strip()
        if line:
            yield number, line


def strip_comments(text):
    """Remove # comments but keep the line structure, so that offsets still give the right line numbers."""
    return "\n".join(line.split("#", 1)[0] for line in text.splitlines())


def block_end(text, open_index):
    """Return the index of the brace that closes the one at open_index (len(text) if unbalanced)."""
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return len(text)


def top_level_blocks(text):
    """Return [(name, line number, body text)] of the top-level `name = { ... }` blocks of comment-stripped text."""
    blocks, position = [], 0
    for m in re.finditer(r"^[ \t]*([A-Za-z_]\w*)[ \t]*=[ \t]*\{", text, re.M):
        if m.start() < position:
            continue
        end = block_end(text, m.end() - 1)
        blocks.append((m.group(1), text.count("\n", 0, m.start()) + 1, text[m.end():end]))
        position = end
    return blocks


def relname(mod, path):
    return f"{mod.name}/{path.relative_to(mod).as_posix()}"


LOC_LINE_RE = re.compile(r'^\s+([A-Za-z0-9_.\-]+):\d*\s+"(.*)"\s*$')


def parse_loc(mod):
    """Return {key: (value, file label, line number)} over the mod's localization files."""
    keys = {}
    for path in loc_files(mod):
        for number, raw in enumerate(read_text(path).splitlines(), 1):
            m = LOC_LINE_RE.match(raw)
            if m:
                keys[m.group(1)] = (m.group(2), relname(mod, path), number)
    return keys


def defined_script_values(mod):
    names = set()
    for path in mod_txt(mod, "script_values"):
        depth = 0
        for _, line in significant(read_text(path)):
            if depth == 0:
                m = re.match(r"^([A-Za-z_]\w*) = ", line)
                if m:
                    names.add(m.group(1))
            depth += line.count("{") - line.count("}")
    return names


def split_fields(value):
    """Split a localization value on the pipes outside [ ] expressions. Raises ValueError on unbalanced brackets."""
    pieces, current, depth = [], [], 0
    for ch in value:
        if ch == "[":
            depth += 1
        elif ch == "]":
            depth -= 1
            if depth < 0:
                raise ValueError("unbalanced ']'")
        if ch == "|" and depth == 0:
            pieces.append("".join(current))
            current = []
        else:
            current.append(ch)
    if depth != 0:
        raise ValueError("unbalanced '['")
    pieces.append("".join(current))
    return pieces


# ---------------------------------------------------------------------------------------------------------------------
# Structure lint (all mod folders)
# ---------------------------------------------------------------------------------------------------------------------
def lint_structure(mod, root):
    problems = []
    desc = (mod / "descriptor.mod").read_bytes()
    if desc.startswith(BOM):
        problems.append(f"{mod.name}/descriptor.mod: must not have a BOM")
    text = desc.decode("utf-8", errors="replace")
    for key in ("name=", "version=", "supported_version="):
        if key not in text:
            problems.append(f"{mod.name}/descriptor.mod: missing {key}")
    for path in sorted(mod.rglob("*")):
        if not path.is_file() or path.name == ".DS_Store" or path.name == "descriptor.mod":
            continue
        rel = path.relative_to(root)
        data = path.read_bytes()
        if path.suffix in (".txt", ".yml"):
            if not data.startswith(BOM):
                problems.append(f"{rel}: missing UTF-8 BOM")
            if b"\r\n" in data:
                problems.append(f"{rel}: CRLF line endings")
            if not path.name.startswith("curia_"):
                problems.append(f"{rel}: file name must start with curia_")
        elif path.suffix == ".gui":
            # GUI files carry no BOM requirement (the game's own do not); keep LF and the curia_ prefix.
            if b"\r\n" in data:
                problems.append(f"{rel}: CRLF line endings")
            if not path.name.startswith("curia_"):
                problems.append(f"{rel}: file name must start with curia_")
        if path.suffix == ".yml":
            body = data[len(BOM):] if data.startswith(BOM) else data
            if not body.startswith(b"l_english:"):
                problems.append(f"{rel}: localization must start with 'l_english:'")
            for number, raw in enumerate(body.decode("utf-8", errors="replace").splitlines(), 1):
                if raw.strip() and not raw.lstrip().startswith("#") and raw[0] in " \t" and not re.match(r" \S", raw):
                    problems.append(f"{rel}:{number}: a localization key must be indented by exactly one space (no tabs)")
    return problems


# ---------------------------------------------------------------------------------------------------------------------
# Check 1: version agreement
# ---------------------------------------------------------------------------------------------------------------------
def load_literals(mod):
    """Return [(file label, line number, literal)] of every debug_log = "..." literal in the on_action files."""
    found = []
    for path in mod_txt(mod, "on_action"):
        for number, line in significant(read_text(path)):
            m = re.match(r'^debug_log = "(.*)"$', line)
            if m:
                found.append((relname(mod, path), number, m.group(1)))
    return found


def check_version_agreement(mod):
    problems = []
    desc = mod / "descriptor.mod"
    m = re.search(r'^version="([^"]*)"', read_text(desc) if desc.is_file() else "", re.M)
    if not m:
        return [f"{mod.name}/descriptor.mod: no version=\"...\" line"]
    desc_version = m.group(1)

    load_versions = []
    for label, number, literal in load_literals(mod):
        lm = LOAD_RE.match(literal)
        if lm:
            load_versions.append((label, number, lm.group(1)))
    if not load_versions:
        problems.append(f"{mod.name}: no LOAD literal 'CURIA1|0|LOAD|<version>' in common/on_action")
    for label, number, version in load_versions:
        if version != desc_version:
            problems.append(f"{label}:{number}: LOAD literal version '{version}' != descriptor.mod version '{desc_version}'")

    loc = parse_loc(mod)
    if BEGIN_KEY not in loc:
        problems.append(f"{mod.name}: localization key {BEGIN_KEY} is not defined (the BEGIN record carries mod_version)")
    else:
        value, label, number = loc[BEGIN_KEY]
        try:
            pieces = split_fields(value)
        except ValueError:
            pieces = []
        if len(pieces) < 5 or pieces[2] != "BEGIN":
            problems.append(f"{label}:{number}: key {BEGIN_KEY}: cannot find the mod_version field of a BEGIN record")
        elif pieces[4] != desc_version:
            problems.append(f"{label}:{number}: key {BEGIN_KEY}: mod_version '{pieces[4]}' != descriptor.mod version '{desc_version}'")

    meta = mod / ".metadata" / "metadata.json"
    if meta.is_file():
        try:
            meta_version = json.loads(read_text(meta)).get("version")
        except ValueError:
            meta_version = None
        if meta_version != desc_version:
            problems.append(f"{mod.name}/.metadata/metadata.json: version '{meta_version}' != descriptor.mod version '{desc_version}'")
    return problems


# ---------------------------------------------------------------------------------------------------------------------
# Check 2: debug_log keys exist and are used
# ---------------------------------------------------------------------------------------------------------------------
KEY_PREFIXES = ("curia_line_", "curia_probe_")


def debug_log_calls(mod):
    """Return [(file label, line number, key or None, literal or None)] over all script files of the mod."""
    calls = []
    for path in all_script_files(mod):
        for number, line in significant(read_text(path)):
            m = re.match(r"^debug_log = (?:\"(.*)\"|([A-Za-z0-9_]+))$", line)
            if m:
                calls.append((relname(mod, path), number, m.group(2), m.group(1)))
    return calls


def check_debug_log_keys(mod):
    problems = []
    loc = parse_loc(mod)
    used = set()
    for label, number, key, _literal in debug_log_calls(mod):
        if key is None:
            continue
        used.add(key)
        if key not in loc:
            problems.append(f"{label}:{number}: debug_log = {key}: key is not defined in the localization")
    for key, (_value, label, number) in sorted(loc.items()):
        if key.startswith(KEY_PREFIXES) and key not in used:
            problems.append(f"{label}:{number}: key {key} is defined but no debug_log uses it")
    return problems


# ---------------------------------------------------------------------------------------------------------------------
# Check 3: every debug_log follows the line counter increment, no inline literals
# ---------------------------------------------------------------------------------------------------------------------
def check_debug_log_counter(mod):
    problems = []
    for label, number, key, literal in debug_log_calls(mod):
        if literal is not None and "/common/on_action/" not in label:
            problems.append(f"{label}:{number}: debug_log with an inline literal; only the LOAD literal in common/on_action is allowed")
        elif literal is not None and not LOAD_RE.match(literal):
            problems.append(f"{label}:{number}: debug_log literal '{literal}' is not the LOAD literal 'CURIA1|0|LOAD|<version>'")
    for path in all_script_files(mod):
        label_file = relname(mod, path)
        previous, effect, depth = None, None, 0
        for number, line in significant(read_text(path)):
            if depth == 0:
                m = re.match(r"^([A-Za-z_]\w*) = \{", line)
                if m:
                    effect = m.group(1)
            m = re.match(r"^debug_log = ([A-Za-z0-9_]+)$", line)
            if m:
                key = m.group(1)
                wanted = SET1_LINE if effect == BEGIN_EFFECT else INC_LINE
                if previous != wanted:
                    problems.append(
                        f"{label_file}:{number}: debug_log = {key} in {effect}: the previous line must be '{wanted}'"
                        f" (found '{previous}')"
                    )
            previous = line
            depth += line.count("{") - line.count("}")
    problems += check_increment_log_pairs(mod)
    return problems


LINES_WRITE_RE = re.compile(r"\b(?:set|change)_global_variable\s*=\s*\{[^{}]*?\bname\s*=\s*curia_export_lines\b")
KEY_LOG_RE = re.compile(r"^[ \t]*debug_log[ \t]*=[ \t]*[A-Za-z_]\w*[ \t]*$", re.M)


def check_increment_log_pairs(mod):
    """Per top-level effect, the writes of curia_export_lines and the debug_log calls must pair one to one. The adjacency
    check alone passes a doubled or an orphaned increment, and either one makes the END count wrong."""
    problems = []
    for path in all_script_files(mod):
        for name, number, body in top_level_blocks(strip_comments(read_text(path))):
            writes = len(LINES_WRITE_RE.findall(body))
            logs = len(KEY_LOG_RE.findall(body))
            if writes != logs:
                problems.append(
                    f"{relname(mod, path)}:{number}: {name}: {writes} write(s) of curia_export_lines but {logs} debug_log"
                    f" call(s); each debug_log needs exactly one increment (the END count)"
                )
    return problems


# ---------------------------------------------------------------------------------------------------------------------
# Check 4: localization grammar
# ---------------------------------------------------------------------------------------------------------------------
def classify(piece):
    """Return ("expr", text) for one [..] expression, ("lit", text) for plain text, ("bad", reason) otherwise."""
    if "[" not in piece and "]" not in piece:
        if any(c in MARKUP_CHARS or ord(c) < 0x20 for c in piece):
            return ("bad", "markup character in a literal field")
        return ("lit", piece)
    if not (piece.startswith("[") and piece.endswith("]")) or piece.count("[") != 1 or piece.count("]") != 1:
        return ("bad", "an expression mixed with literal text or more than one expression in a field")
    inner = piece[1:-1]
    if any(c in MARKUP_CHARS or ord(c) < 0x20 for c in inner):
        return ("bad", "markup character inside an expression")
    if inner.count("|") > 1 or (inner.count("|") == 1 and not re.search(r"\|[0-9]$", inner)):
        return ("bad", "a pipe inside an expression other than the number format suffix")
    return ("expr", piece)


def int_script_value(piece):
    """Return (True, script value name or None) if the expression is a proven integer form."""
    for pattern in INT_FORMS:
        m = pattern.match(piece)
        if m:
            return True, (m.group(1) if m.groups() else None)
    return False, None


def field_problem(kind, piece):
    """Return None if the piece fits the field kind, else the reason."""
    cls, text = classify(piece)
    if cls == "bad":
        return text
    what = kind[0]
    if what == "none":
        return None if (cls == "lit" and text == "none") else f"expected the literal 'none', found '{piece}'"
    if what == "lit":
        return None if (cls == "lit" and text == kind[1]) else f"expected the literal '{kind[1]}', found '{piece}'"
    if what == "token":
        return None if (cls == "lit" and TOKEN_RE.match(text)) else f"expected a version token, found '{piece}'"
    if what == "int":
        if cls == "expr" and int_script_value(piece)[0]:
            return None
        return f"expected a proven integer expression, found '{piece}'"
    if what == "text":
        if cls == "expr" and piece in TEXT_FORMS:
            return None
        return f"expected a proven text expression, found '{piece}'"
    return f"unknown field kind {kind}"


def match_record(rtype, fields):
    """Return (form id, None) for the form of this record type that fits, else (None, reason). When nothing fits, the
    reason names the closest form: among the forms whose literal words all occur in the record, the one that got
    furthest (or, for a wrong field count, the one with the nearest count)."""
    candidates = [(fid, kinds) for fid, t, kinds in FORMS if t == rtype]
    if not candidates:
        return None, f"record type {rtype} is not in the table of docs/wire-format.md section 4"

    def words(kinds):
        return [k[1] for k in kinds if k[0] == "lit"] + ["none" for k in kinds if k[0] == "none"]

    compatible = [(fid, kinds) for fid, kinds in candidates if all(w in fields for w in words(kinds))]
    best_reason, best_score = None, None
    for fid, kinds in compatible or candidates:
        if len(kinds) != len(fields):
            reason = f"form '{fid}' has {len(kinds)} fields after the type, found {len(fields)}"
            score = -abs(len(kinds) - len(fields))
        else:
            reason, score = None, 1
            for index, (kind, piece) in enumerate(zip(kinds, fields), 1):
                problem = field_problem(kind, piece)
                if problem:
                    reason, score = f"form '{fid}', field {index}: {problem}", score + index - 1
                    break
            if reason is None:
                return fid, None
        if best_score is None or score > best_score:
            best_reason, best_score = reason, score
    return None, best_reason


def source_problems(where, form, fields):
    problems = []
    for index, piece in enumerate(fields, 1):
        required = EXPECTED_SOURCE.get((form, index))
        if required is None:
            continue
        if required.startswith("["):
            ok = piece == required
        else:
            ok = int_script_value(piece)[1] == required
        if not ok:
            problems.append(f"{where}: form '{form}', field {index} must read {required}, found '{piece}'")
    return problems


def check_localization_grammar(mod):
    """Return (problems, covered form ids). A key is checked when it starts with curia_line_ or curia_probe_."""
    problems, covered = [], set()
    defined = defined_script_values(mod)
    for key, (value, label, number) in sorted(parse_loc(mod).items()):
        if not key.startswith(KEY_PREFIXES):
            continue
        where = f"{label}:{number}: key {key}"
        try:
            pieces = split_fields(value)
        except ValueError as exc:
            problems.append(f"{where}: {exc}")
            continue
        if len(pieces) < 3 or pieces[0] != "CURIA1" or pieces[1] != SNAP_EXPR:
            problems.append(f"{where}: must start with 'CURIA1|{SNAP_EXPR}|<TYPE>|'")
            continue
        rtype = pieces[2]
        if not TYPE_RE.match(rtype):
            problems.append(f"{where}: record type '{rtype}' does not match [A-Z][A-Z0-9_]*")
            continue
        fields = pieces[3:]
        for piece in fields:
            cls, text = classify(piece)
            if cls == "bad":
                problems.append(f"{where}: {text} ('{piece}')")
            elif cls == "expr":
                _ok, name = int_script_value(piece)
                if name is not None and name not in defined:
                    problems.append(f"{where}: refers to the script value '{name}', which common/script_values does not define")
        if key.startswith("curia_probe_"):
            if rtype != "PROBE":
                problems.append(f"{where}: a curia_probe_ key must be a PROBE record, found {rtype}")
            continue
        if rtype == "PROBE":
            problems.append(f"{where}: PROBE records belong in curia_probe_ keys")
            continue
        texts = [i for i, f in enumerate(fields) if f in TEXT_FORMS]
        if len(texts) > 1:
            problems.append(f"{where}: more than one text expression in a record")
            continue
        if texts and texts[0] != len(fields) - 1:
            problems.append(f"{where}: the text expression must be the last field, but fields follow it")
            continue
        form, reason = match_record(rtype, fields)
        if form is None:
            problems.append(f"{where}: {reason}")
        else:
            covered.add(form)
            problems += source_problems(where, form, fields)
    return problems, covered


def check_table_coverage(mod):
    """Every form of the table needs at least one key (each none form and enum value has its own key)."""
    _problems, covered = check_localization_grammar(mod)
    missing = [fid for fid, _t, _k in FORMS if fid not in covered]
    return [f"{mod.name}: no curia_line_ localization key for the record form '{fid}'" for fid in missing]


# ---------------------------------------------------------------------------------------------------------------------
# Check 5: global variables
# ---------------------------------------------------------------------------------------------------------------------
GLOBAL_CALL_RE = re.compile(r"\b(set|change)_global_variable\s*=\s*\{")
GLOBAL_NAME_RE = re.compile(r"\bname\s*=\s*([A-Za-z0-9_]+)")


def global_writes_in(text):
    """Return [(kind 'set'/'change', name, offset)] over comment-stripped text; the block may span several lines."""
    writes = []
    for m in GLOBAL_CALL_RE.finditer(text):
        name = GLOBAL_NAME_RE.search(text[m.end():block_end(text, m.end() - 1)])
        if name:
            writes.append((m.group(1), name.group(1), m.start()))
    return writes


def global_writes(mod):
    """Return [(kind 'set'/'change', name, file label, line number)] over every script file of the mod."""
    writes = []
    for path in all_script_files(mod):
        text = strip_comments(read_text(path))
        for kind, name, offset in global_writes_in(text):
            writes.append((kind, name, relname(mod, path), text.count("\n", 0, offset) + 1))
    return writes


def check_globals_read(mod):
    read = set()
    for path in mod_txt(mod, "script_values"):
        read.update(re.findall(r"global_var:([A-Za-z0-9_]+)", "\n".join(line for _, line in significant(read_text(path)))))
    problems, reported = [], set()
    for _kind, name, label, number in global_writes(mod):
        if name not in read and name not in reported:
            reported.add(name)
            problems.append(
                f"{label}:{number}: global variable {name} is set but no script value in common/script_values reads"
                f" global_var:{name} (the engine reports it at load)"
            )
    return problems


def effect_body(text, effect):
    """Return the text between the braces of the top-level effect `effect`, or None."""
    m = re.search(r"^" + re.escape(effect) + r"\s*=\s*\{", text, re.M)
    if not m:
        return None
    depth, start = 0, m.end() - 1
    for index in range(start, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:index]
    return None


def check_counters_reset(mod):
    body = None
    for path in mod_txt(mod, "scripted_effects"):
        found = effect_body(read_text(path), BEGIN_EFFECT)
        if found is not None:
            body = found
    if body is None:
        return [f"{mod.name}: the effect {BEGIN_EFFECT} is not defined in common/scripted_effects"]
    reset = {name for kind, name, _offset in global_writes_in(strip_comments(body)) if kind == "set"}
    problems, reported = [], set()
    for kind, name, label, number in global_writes(mod):
        if kind == "change" and name not in reset and name not in reported:
            reported.add(name)
            problems.append(f"{label}:{number}: counter {name} is changed but not set in {BEGIN_EFFECT} (every counter is reset per frame)")
    return problems


# ---------------------------------------------------------------------------------------------------------------------
# Check 6: wiring of the sections (only for a complete mod; a mini mod in a test has no sections)
# ---------------------------------------------------------------------------------------------------------------------
RUN_EFFECT = "curia_export_run"
SECTION_ORDER = ["begin", "fin", "heir", "vassals", "wars", "claims", "factions", "end"]
# section effect -> the ordered_* iterator whose counting and writing pass must agree
TWO_PASS = {
    "curia_export_vassals": "ordered_vassal",
    "curia_export_claims": "ordered_claim",
    "curia_export_factions": "ordered_targeting_faction",
}
PARAM_KEYS = {"max", "check_range_bounds", "order_by", "limit", "explicit", "pressed"}
COUNTED_GLOBALS = ["curia_vassals_listed", "curia_wars_total", "curia_claims_listed", "curia_factions_listed"]


def statements(body):
    """Split a block body into its top-level statements (a one-line statement or a braced one), whitespace normalised."""
    result, current, depth = [], [], 0
    for line in body.splitlines():
        if not line.strip():
            continue
        current.append(line.strip())
        depth += line.count("{") - line.count("}")
        if depth <= 0:
            result.append(re.sub(r"\s+", " ", " ".join(current)))
            current, depth = [], 0
    if current:
        result.append(re.sub(r"\s+", " ", " ".join(current)))
    return result


def check_wiring(mod):
    problems = []
    effects = {}
    for path in mod_txt(mod, "scripted_effects"):
        for name, number, body in top_level_blocks(strip_comments(read_text(path))):
            effects[name] = (relname(mod, path), number, body)

    # the order of the sections
    if RUN_EFFECT not in effects:
        problems.append(f"{mod.name}: the effect {RUN_EFFECT} is not defined in common/scripted_effects")
    else:
        label, number, body = effects[RUN_EFFECT]
        calls = re.findall(r"^[ \t]*(curia_export_\w+)[ \t]*=[ \t]*yes\b", body, re.M)
        wanted = ["curia_export_" + w for w in SECTION_ORDER]
        with_probes = wanted[:-1] + ["curia_export_probes", wanted[-1]]
        if calls not in (wanted, with_probes):
            problems.append(
                f"{label}:{number}: {RUN_EFFECT} calls {calls}; the sections must run in the order of docs/wire-format.md"
                f" section 4: {wanted} (with curia_export_probes between factions and end when probing)"
            )
        for call in calls:
            if call not in effects:
                problems.append(f"{label}:{number}: {RUN_EFFECT} calls {call}, which no scripted effect defines")

    # identical iterator parameters in the counting and the writing pass
    for effect, iterator in TWO_PASS.items():
        if effect not in effects:
            problems.append(f"{mod.name}: the effect {effect} is not defined in common/scripted_effects")
            continue
        label, number, body = effects[effect]
        passes = []
        for m in re.finditer(r"\b" + iterator + r"[ \t]*=[ \t]*\{", body):
            inner = body[m.end():block_end(body, m.end() - 1)]
            passes.append(sorted(s for s in statements(inner) if s.split(" ", 1)[0] in PARAM_KEYS))
        if len(passes) != 2:
            problems.append(f"{label}:{number}: {effect} must run {iterator} twice (counting and writing pass), found {len(passes)}")
        elif passes[0] != passes[1]:
            problems.append(f"{label}:{number}: {effect}: the counting and the writing pass of {iterator} have different parameters")
        elif not any(s.startswith("max ") for s in passes[0]):
            problems.append(f"{label}:{number}: {effect}: {iterator} has no max (the cap of the section)")

    # every counting global is changed somewhere (begin only resets it)
    changed = {name for kind, name, _l, _n in global_writes(mod) if kind == "change"}
    for name in COUNTED_GLOBALS:
        if name not in changed:
            problems.append(f"{mod.name}: counting global {name} is never changed, so its header would always read 0")
    return problems


# ---------------------------------------------------------------------------------------------------------------------
def lint_export(mod, coverage=True):
    problems = []
    problems += check_version_agreement(mod)
    problems += check_debug_log_keys(mod)
    problems += check_debug_log_counter(mod)
    problems += check_localization_grammar(mod)[0]
    if coverage:
        problems += check_table_coverage(mod)
        problems += check_wiring(mod)
    problems += check_globals_read(mod)
    problems += check_counters_reset(mod)
    return problems


def lint_all(root):
    """Return (problems, number of mod folders)."""
    problems = []
    mods = sorted(p.parent for p in root.glob("*/descriptor.mod")) if root.exists() else []
    for mod in mods:
        problems += lint_structure(mod, root)
        if mod.name == CURIA_MOD:
            problems += lint_export(mod)
    return problems, len(mods)


def main():
    problems, count = lint_all(REPO_ROOT / "mod")
    if problems:
        print("\n".join(problems))
        return 1
    print(f"mod lint ok ({count} mod folder(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
