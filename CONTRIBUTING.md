# Contributing to Curia

Thanks for helping. Curia is pre-alpha with a single maintainer, so small, focused
changes that match the existing design documents are the easiest to review. By taking
part you agree to the [Code of Conduct](CODE_OF_CONDUCT.md).

## Before you start

- Read the documents in [`docs/`](docs/). They are the source of truth:
  `requirements.md` (what and why, decisions D1 to D14, repository standards),
  `architecture.md`, `milestones.md`, and `research-notes.md` (sourced facts and open
  questions).
- For anything larger than a small fix, open an issue first so the approach can be
  agreed before you spend time on it.
- Ask questions and report bugs as described in [SUPPORT.md](SUPPORT.md). Security
  problems go through [SECURITY.md](SECURITY.md), never a public issue.

## Hard rules

1. **No Paradox content in the repository.** Do not commit game script files,
   `script_docs` or `dump_data_types` output, raw `debug.log` or `error.log` captures,
   or screenshots and assets taken from the game. Test fixtures may contain only
   Curia's own marker lines. Keep local reference copies in `reference/`, which is
   gitignored.
2. **No Voices of the Court code.** Do not copy or closely port code, script files or
   assets from Voices of the Court (its app is GPL-3.0 and its mod is CC BY-SA 4.0,
   both incompatible with Curia's MIT license). Concepts are fine; text and code are not.
3. **CK3 script comes from ground truth, not memory.** Write script against the game's
   installed files and your own local `script_docs` / `dump_data_types` dumps, and say
   which game version you checked. If you cannot run the game, say so in the PR and
   mark what is untested.
4. **No launch options or console in the user path.** Curia must never require or
   suggest launching CK3 in debug mode or using the console: both disable
   achievements. Generating dumps and console experiments happen only in a throwaway
   game on your own machine.
5. **Read-only toward the game.** The app never writes to CK3 files; the mod writes
   nothing to `error.log` in normal use.
6. **No model IDs in source.** Models are discovered from the provider's models
   endpoint. Examples belong in documentation only.

## Development setup

You need:

- CMake 3.25 or newer, and Ninja on macOS and Linux (the Windows preset uses the Visual
  Studio 2022 generator).
- A C++20 compiler: Xcode command line tools on macOS, Visual Studio 2022 (or Build Tools
  2022) with the C++ desktop workload on Windows, GCC 13 or Clang on Linux.
- A [vcpkg](https://github.com/microsoft/vcpkg) checkout, bootstrapped
  (`bootstrap-vcpkg.sh` or `.bat`), with `VCPKG_ROOT` pointing at it. Dependencies are
  declared in `vcpkg.json` (pinned by `builtin-baseline`) and built in manifest mode. This
  is the only way third-party code enters the project. The vcpkg triplets in
  `cmake/triplets/` are release-only, so only Release and RelWithDebInfo builds link.
- Ubuntu 24.04 system packages for SDL3 (the same list CI uses):
  `sudo apt-get install pkg-config libx11-dev libxext-dev libxft-dev libxkbcommon-dev
  libwayland-dev wayland-protocols libegl1-mesa-dev libibus-1.0-dev xvfb xauth`
- Python 3, for the helper scripts under `tools/`. The `.sh` scripts need bash and `jq`
  (Git Bash on Windows).

### Before you push

Formatting uses a pinned clang-format (a different version formats differently), so install
it from the requirements file and run the checks CI runs:

```sh
python3 -m venv .venv-tools && .venv-tools/bin/pip install -r tools/requirements-dev.txt
CLANG_FORMAT=.venv-tools/bin/clang-format tools/ci/check_format.sh --fix   # --fix rewrites files
tools/ci/scan_patterns.sh      # no Paradox content, no keys, no home paths
tools/ci/check_versions.sh     # CMake, vcpkg.json and CHANGELOG agree
python3 tools/ci/lint_mod.py   # structure of the mod folders
```

## Build and test

Presets are named per platform: `macos-arm64`, `macos-x64`, `linux-x64`,
`windows-x64`.

```sh
cmake --preset macos-arm64
cmake --build --preset macos-arm64
ctest --preset macos-arm64            # unit, smoke and idle tests
ctest --preset macos-arm64-network    # one HTTPS handshake to a public host (no key sent)
```

The smoke and idle tests open a small window, so run `ctest` in a graphical session (on
headless Linux: `xvfb-run -a ctest --preset linux-x64`) and do not move the mouse over
the window while `app_idle` runs. Unit tests use [doctest](https://github.com/doctest/doctest). Add a test with every
behaviour change. Parsing code takes untrusted text, so keep it a pure function that
can be fuzzed.

The mod can be validated locally with ck3-tiger, run by you against your own game
install. **CI never touches game files**, and neither should any workflow you add.
Expect some false positives while ck3-tiger targets an older game version. After a
run in game, check CK3's `error.log` for entries whose names start with `curia_`.

Mod identifiers are prefixed `curia_`, and the mod must not override any vanilla GUI
file.

## Commits and pull requests

### Commit convention

Commits follow [Conventional Commits 1.0.0](https://www.conventionalcommits.org/en/v1.0.0/):

```
<type>(<scope>): <summary in the imperative, lower case, no final period>
```

Types: `feat`, `fix`, `docs`, `refactor`, `perf`, `test`, `build`, `ci`, `chore`.
The scope is optional free text; common ones are `mod`, `app`, `ui`, `provider`,
`export`, `persona`, `release`. Mark a
breaking change with `!` or a `BREAKING CHANGE:` footer.

```
feat(app): add log watcher with truncation handling
fix(mod): stop setting a variable that script never reads
docs: record M3 flush-latency measurements
ci: pin actions/checkout to a commit SHA
feat(provider)!: rename the base_url setting to endpoint
```

### Pull request flow

1. Fork the repository (or create a branch if you have access) and branch from the
   default branch. Do not commit to the default branch directly.
2. Keep the PR to one logical change. Fill in the PR template: summary, linked issue,
   type, what you tested on, the no-secrets confirmation, and the changelog checkbox.
3. Update [`CHANGELOG.md`](CHANGELOG.md) under `[Unreleased]` for user-visible
   changes, following [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
4. Wait for the **`CI result`** check to pass. It summarises build and test on Windows,
   macOS and Linux, lint and the other jobs. The separate **`PR title`** check must pass
   too.
5. PRs are **squash-merged**, so the **PR title becomes the commit message** and must be a
   valid Conventional Commit. The `PR title` check lints it and re-runs when you edit the
   title. Individual commit messages inside the
   PR are not kept, but write them clearly anyway.

Versions follow [SemVer 2.0.0](https://semver.org/spec/v2.0.0.html) with tags like
`v0.1.0`; the version stays `0.y.z` until the wire protocol and config schema are
declared stable. There is one version for the app and the mod. The wire-protocol
version (`CURIA1`) is separate and lives inside the data.

## Docs first

The documents in `docs/` lead and the code follows. When a change alters a decision,
a requirement or a milestone's acceptance criteria, edit the relevant document in the
same PR (or an earlier one) and say why. Do not let the code and the documents
disagree.

### Spike results

A spike is a question answered by running something, not by estimating. Facts that
need the game, or hardware the maintainer does not have, are recorded as results, not
guesses.

- Write results to `docs/spikes/<name>.md`: date, game and OS versions, what was run,
  what was observed, and what was not measured.
- Do not paste raw game logs. Summarise, or quote only Curia's own marker lines.
- Then fold the answers back into `docs/research-notes.md`, moving the item from
  "spike" to "verified".
- Mark every claim about Windows, Linux or Intel Macs as unverified until a tester
  reports it. The maintainer can test only on an Apple silicon Mac.

## Secrets and test-key hygiene

- Never commit a real API key, token or password, and never put one in an issue, PR,
  log excerpt or screenshot. Push protection and secret scanning are enabled on the maintainer's repository (see
  Maintainer notes), and CI scans for key-shaped strings.
- Tests, fixtures and docs use **obviously fake keys** (for example
  `test-key-not-real`) and a local mock server. No test sends a request that needs an API key. The one network test (`ctest --preset
  <os>-network`) makes a single TLS handshake to a public host to prove certificate
  validation works.
- Code must not log keys, put them on a command line, or write them to config files.
  Secrets live in the OS secure store (plus a development-only environment variable
  in early milestones that is never shipped or logged). Add a redaction test when you
  touch logging.
- If you committed a secret by accident, revoke it first, then tell the maintainer.

## Licensing of contributions

Curia is under the [MIT license](LICENSE), for the app and the mod. By submitting a
contribution you agree that it is licensed under the same MIT terms (inbound equals
outbound). There is no separate contributor agreement. Only submit work you wrote or
have the right to submit under the MIT license, and keep third-party code out of the
tree except through `vcpkg.json`. Add `// SPDX-License-Identifier: MIT` as the first
line of new source files.

## Maintainer notes

These repository settings cannot be committed as files. The maintainer sets them
once; they are recorded here so they live in the repository. Source:
`docs/milestones.md`, M1b. **Order matters:** do steps 3 and 4 before the first push,
then push, wait for one CI run, and only then create the ruleset (a required check can
be picked only after it has run once).

1. **Ruleset on the default branch (Active), after the first CI run.**
   - Require a pull request, with **0 required approvals** (a solo author cannot
     approve their own PR).
   - Require the status checks **`CI result`** and **`PR title`**.
   - Require linear history.
   - Block force pushes and deletions.
   - Require conversations to be resolved.
   - Allow **squash merges only**.
   - Keep admin bypass for emergencies only.
2. **Settings, Actions, General.** Default `GITHUB_TOKEN` permissions read-only;
   require actions to be pinned to a full commit SHA.
3. **Settings, Advanced Security.** Enable private vulnerability reporting, the
   dependency graph, Dependabot alerts and security updates, and secret scanning with
   push protection. Enable CodeQL for C/C++ as a scheduled scan, not a PR gate.
4. **Labels.** Create a small set: `bug`, `enhancement`, `documentation`,
   `needs-triage`, `tester-report`, `platform:*`, `area:*`, `good first issue`,
   `help wanted`, `breaking-change`, `skip-release`, `dependencies`, `ci`, `security`.
5. **Two-factor authentication** on the maintainer account. Release immutability is
   enabled in M7, before the first public release.
6. **Verify** that Insights, Community Standards shows every item green.

Other maintainer conventions: third-party GitHub Actions are pinned by commit SHA,
workflows default to read-only `permissions:` and raise them per job, no workflow
uses `pull_request_target`, and no required workflow uses `paths:` filters (a
skipped required check stays pending forever).
