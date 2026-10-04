# Curia

An in-game AI advisor for Crusader Kings III. You open a chat window over the game,
ask about your realm ("what should I focus on?", "is my heir safe?"), and the advisor
answers using live data about your game exported by a mod.

> **Status: pre-alpha. There is no release, no installable app and no working mod
> yet.** The repository currently holds the design documents, the findings of the
> feasibility tests, and the project skeleton (an empty SDL3 and Dear ImGui window).
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

Only the skeleton builds today. You need CMake 3.25 or newer, Ninja (macOS and Linux), a
C++20 compiler (Xcode command line tools; Visual Studio 2022 with the C++ desktop workload;
GCC 13 or Clang on Linux) and a [vcpkg](https://github.com/microsoft/vcpkg) checkout that
you have bootstrapped (`bootstrap-vcpkg.sh` or `.bat`).

```sh
export VCPKG_ROOT=/path/to/vcpkg          # Windows: set VCPKG_ROOT=C:\path\to\vcpkg

cmake --preset macos-arm64                # or macos-x64, linux-x64, windows-x64
cmake --build --preset macos-arm64
ctest --preset macos-arm64                # opens a small window for a few seconds
```

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
