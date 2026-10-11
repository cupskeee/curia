# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).
Until the wire protocol and the config schema are declared stable, versions stay
at `0.y.z` and anything may change between minor versions.

## [Unreleased]

### Added

- Design documents under `docs/`: requirements, architecture, milestones and
  research notes with sourced facts, assumptions and open questions.
- Milestone 0 feasibility findings (`docs/spikes/`), measured by hand on
  macOS 26.5.1 with Crusader Kings III 1.20.0.3. Nothing from this milestone is
  shipped. Main results: `debug_log` writes to `debug.log` on a normal launch,
  also in Ironman, with achievements still shown as available; a launch with
  `-debug_mode` showed achievements as not available; names, numbers and
  opinions resolve inside `debug_log` with `THIS`-based forms. (These were
  feasibility tests with a throwaway mod, `mod/curia-m0/`, which is not the
  Curia mod and must not be installed.)
- Build skeleton: CMake presets and a pinned vcpkg manifest, an SDL3 + Dear ImGui
  window with an event-driven loop, a thin platform layer, unit tests (doctest),
  a headless smoke test, an idle test and an HTTPS smoke test.
- CI on Windows, macOS (arm64 and a cross-built x86_64 merged into a universal
  binary) and Linux, with lint, sanitizers and the `CI result` aggregate check.
- Repository standards: `README.md`, `LICENSE` (MIT), `CONTRIBUTING.md`,
  `CODE_OF_CONDUCT.md` (Contributor Covenant 3.0), `SECURITY.md`, `SUPPORT.md`
  and this changelog.

### Changed

- The Curia mod (`mod/curia/`, now 0.2.1) writes the approved `CURIA1` export
  format of `docs/wire-format.md` instead of the spike layout: records with
  ids, section headers with totals, numeric date and mod version in `BEGIN`,
  the net monthly balance (income minus expenses, approximately as the top bar
  shows it, ±0.2) in tenths of a gold piece, explicit claims marked `strong` or `weak`, and
  faction groups. The format was frozen on 2026-10-11 after the pre-freeze
  in-game check, and the earlier spike output is not readable by the new
  format. `tools/ci/lint_mod.py` now checks the format's invariants.

<!--
Compare links are added when the first tag exists, for example:

[Unreleased]: https://github.com/cupskeee/curia/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/cupskeee/curia/releases/tag/v0.1.0
-->
