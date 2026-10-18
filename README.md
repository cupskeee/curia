# Curia

An in-game AI advisor for Crusader Kings III. You open a chat window over the game,
ask about your realm ("what should I focus on?", "is my heir safe?"), and the advisor
answers using live data about your game exported by a mod.

> **Status: pre-alpha. There is no release, no installable app and no working mod
> yet.** The repository currently holds the design documents, the findings of the
> feasibility tests, and the app in progress (the chat in an overlay over the game on
> macOS, a normal window on Windows and Linux, and headless test modes).
> Everything below except "Build from source" describes the plan, not what you can
> run today.

Curia is for players who find CK3 overwhelming and want a second opinion, not for
playing the game for them. In version 1 it is read-only: it never sends commands back
into the game.

## How it works (planned)

1. **Mod.** A small CK3 mod adds an "Advisor" button to the game's UI. Pressing it
   writes a compact summary of your realm to CK3's own `debug.log` (gold and income,
   heir and succession, vassals and their opinions, factions, wars, claims). Each line
   carries a versioned marker, `CURIA1|`, so it can be told apart from the game's other
   output. This works on a normal launch, with no launch options.
2. **Companion app.** A small native app (C++20, SDL3, Dear ImGui) watches the log, picks
   up the marker lines, and opens a chat overlay over the game.
3. **Language model.** Your question and the realm summary go to the model endpoint you
   configure: any OpenAI-compatible server (OpenAI, OpenRouter, DeepSeek, Ollama, LM
   Studio, llama.cpp server) or the Anthropic Messages API. Answers stream into the
   chat.

The app and the mod are one project with one version number.

### What the feasibility tests showed

On 2026-10-04, on one Mac (macOS 26.5.1, CK3 1.20.0.3), a throwaway test mod confirmed
that `debug_log` writes to `debug.log` on a normal launch, also in an Ironman game, with
achievements still shown as available. Names, numbers and opinions can be exported.
Details: [`docs/spikes/m0-results.md`](docs/spikes/m0-results.md). Nothing has been
verified on Windows or Linux yet.

## Rules for players

- **Never launch CK3 in debug mode for Curia.** Curia does not need it. In testing, a
  debug-mode launch showed achievements as "Not available" from the start.
- **Never use the CK3 console for Curia.** Console use disables achievements, and
  Curia never asks for it. If the advisor goes quiet, the planned remedy is to restart
  CK3 (the log can stop growing during a long session), not to run a console command.

## Platforms

The minimum supported systems match Crusader Kings III's own minimum requirements:

| OS | Minimum | Status |
|---|---|---|
| Windows | Windows 10, 64-bit | planned, full support |
| macOS | macOS 15 Sequoia (Apple silicon; an Intel build is planned but unverified) | planned, full support |
| Linux | Ubuntu 24.04 LTS | planned; X11 or XWayland (run with `SDL_VIDEO_DRIVER=x11`); native Wayland is best effort |

Steam Deck Gaming Mode is not supported. The maintainer can test only on an Apple
silicon Mac. Windows and Linux builds are checked by automated CI and by community
testers, so expect rough edges there until testers report back. See
[SUPPORT.md](SUPPORT.md) for the tester report form.

## Privacy (design commitments, not yet implemented)

The app does not yet talk to any language model, store any key or read any log. These are
the rules the finished app will follow:

- Curia will send **no telemetry** and make no network requests of its own except to the
  LLM endpoint you configure (and that endpoint's model-list request).
- Your **realm data and questions will go only to that endpoint**. What it does with them is
  governed by that provider's terms. If you want nothing to leave your machine, point
  Curia at a local server.
- **API keys** will be stored in the operating system's secure store: Windows Credential
  Manager, macOS Keychain, or libsecret on Linux. If Linux has no keyring service,
  the key will be kept in memory for the session only and not written to disk. Keys
  will never be put in config files, logs or command lines.
- Curia will only read `debug.log`. It will never write to any CK3 file.

## Install (not yet available)

None of the following exists yet. There are no downloads; do not install anything
that claims to be Curia from another source. The steps below are the plan and will
change.

- **Windows:** download a zip from GitHub Releases, unpack it, run `curia.exe`. The
  first releases will be unsigned, so Windows SmartScreen will warn: choose "More
  info", then "Run anyway".
- **macOS:** download a dmg from GitHub Releases and drag the app to Applications.
  Until a notarized build exists, the app will be un-notarized, and macOS will block
  the first launch. Open it once, then go to System Settings, Privacy & Security, and
  choose "Open Anyway" (right-click, Open no longer works for this on macOS 15).
- **Linux:** an AppImage and a plain `tar.gz`. Run under X11 or XWayland.
- **Mod:** a zip in each release; distribution through the Steam Workshop is planned
  but not final.
- **API key:** entered in the app's settings.
- **Verifying a download:** each release is planned to ship a `SHA256SUMS` file and
  build-provenance attestations that you can check with `gh attestation verify`.

## Build from source

The app is a work in progress (milestone M4). You need CMake 3.25 or newer, Ninja (macOS
and Linux), a C++20 compiler (Xcode command line tools; Visual Studio 2022 with the C++
desktop workload; GCC 13 or Clang on Linux) and a
[vcpkg](https://github.com/microsoft/vcpkg) checkout that you have bootstrapped
(`bootstrap-vcpkg.sh` or `.bat`).

```sh
export VCPKG_ROOT=/path/to/vcpkg          # Windows: set VCPKG_ROOT=C:\path\to\vcpkg

cmake --preset macos-arm64                # or macos-x64, linux-x64, windows-x64
cmake --build --preset macos-arm64
ctest --preset macos-arm64                # opens a small window for a few seconds
```

To see the chat window without the game or a key, `curia --smoke --screenshot shot.bmp`
draws a canned conversation and saves the last frame. To try it with the mock server and a
fake game log (development builds), start the fake CK3 and the mock in two terminals, then
the window in a third:

```sh
python3 tools/fake_ck3/fake_ck3.py --stdin        # prints LOG <path>; type "frame" to click Advisor
python3 tools/mock_llm/mock_llm.py --ignore-stdin # prints LISTEN 127.0.0.1:<port>

export CURIA_ANTHROPIC_API_KEY=sk-ant-FAKE-try-the-window-0000   # a fake key: the mock needs one
export CURIA_MODEL=mock-model-1
build/macos-arm64/app/curia --log <LOG path> --test-endpoint http://127.0.0.1:<port>
```

With the real service, type the key at a hidden prompt in your own terminal, so that it is
neither shown nor kept in the shell history (never put a real key into a command, a file or
a chat), set the model, and run `curia` with no flag: it reads `debug.log` from CK3's
folder in Documents and shows the realm once you click Advisor in the game. The full steps
are in [docs/spikes/m4-quality-check.md](docs/spikes/m4-quality-check.md).

```sh
read -s "CURIA_ANTHROPIC_API_KEY?API key (nothing is shown): "   # zsh; bash: read -rs -p "API key: " CURIA_ANTHROPIC_API_KEY
export CURIA_ANTHROPIC_API_KEY; echo
export CURIA_MODEL=<model>                # a model ID from the provider's list
build/macos-arm64/app/curia
```

On macOS the chat opens in an overlay that floats over CK3, also over its Fullscreen mode, without
a Dock icon (`--window plain` gives a normal window instead). It appears when Curia starts and
each time you click Advisor in the game, on CK3's display. **Ctrl+Cmd+K** shows and hides it;
**Esc** hides it (what you typed stays, and an answer keeps arriving); Cmd+. stops an answer. When
you open it from the game, with the hot key or from the menu-bar icon, the overlay is made the key
window so that you can type at once; whether the keys then really go to it and not to the game is
still to be checked in the M4 testing (`--overlay-focus click` waits for a click on it instead).
The overlay shown at start-up does not keep the keys: if starting Curia made it the active app, it
hands them back a quarter of a second later. The menu-bar icon has "Show Curia", "Hide Curia" and
"Quit Curia" (it is left out if CK3 is in front when Curia starts); over a game in Fullscreen the
menu bar may not appear, so the in-game button and the hot key are the main controls. On Windows
the chat is a normal window that stays on top, with a tray icon; on Linux the same window without
one. There Esc minimises the window, its close button quits Curia, and there is no hot key yet.

The build also makes `build/macos-arm64/app/Curia.app` (ad-hoc signed, not notarised), which
macOS treats as an app of its own rather than part of your terminal. During development you can
start it with `open` from a terminal where the variables are exported:

```sh
open -g build/macos-arm64/app/Curia.app --stderr <file> --args --log <LOG path>
```

`open` hands its own environment to the app, so the exported variables arrive without any `--env`
and the key never appears on a command line; `--env NAME` without a value sets the variable to an
empty value instead (tested on macOS 26.5.1, `docs/research-notes.md` §2.17). `-g` keeps `open`
from bringing Curia to the front (it may still become active for a moment, as above). The app runs
in `/`, so give absolute paths (after `--args` as well), and its error messages reach only the
`--stderr` file. If Curia is already running, `open` starts no second copy: quit it first (from
the menu-bar icon, or `kill` its process). The app log (`~/Library/Logs/Curia/curia.log`) has a
line `dev_config key=set|unset|unusable model=...` that says which variables arrived, never a
value. How the M4 test sittings start Curia is settled with their session sheet.

The first configure downloads and builds the dependencies through vcpkg's manifest
mode, which takes a while. The tests need a display (on headless Linux use
`xvfb-run -a ctest --preset linux-x64`), and the idle test fails if you move the mouse
over its window. Only Release and RelWithDebInfo builds are supported (the vcpkg
triplets are release-only). The Ubuntu package list and more detail are in
[CONTRIBUTING.md](CONTRIBUTING.md).

## Contributing, security, license

- Contributions are welcome: [CONTRIBUTING.md](CONTRIBUTING.md). Participation is
  governed by the [Code of Conduct](CODE_OF_CONDUCT.md).
- Questions and bug reports: [SUPPORT.md](SUPPORT.md).
- Security problems: report privately, see [SECURITY.md](SECURITY.md). Do not open a
  public issue.
- Changes: [CHANGELOG.md](CHANGELOG.md).
- License: [MIT](LICENSE), for both the app and the mod.
- Design documents: [`docs/`](docs/) (requirements, architecture, milestones,
  research notes).

## Credit and disclaimer

Inspired by Voices of the Court.

Curia is an unofficial fan project. It is not affiliated with, endorsed by or
sponsored by Paradox Interactive. Crusader Kings is a trademark of Paradox
Interactive AB. Curia contains no Paradox game content.
