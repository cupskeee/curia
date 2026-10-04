# Security policy

## Supported versions

Curia is pre-alpha and has no release yet. While the version is `0.y.z`, only the
**latest release** receives security fixes. Until the first release exists, the
default branch is the only supported code.

## Reporting a vulnerability

Report it privately through GitHub's private vulnerability reporting:

<https://github.com/cupskeee/curia/security/advisories/new>

Do not open a public issue, pull request or discussion for a suspected vulnerability,
and do not post details elsewhere before a fix is available.

Please include:

- what the problem is and what an attacker could do with it,
- the affected version or commit, and your operating system,
- steps to reproduce, or a proof of concept that does not touch anyone else's data.

**Never paste a real API key or a full log file into a report.** If a key was
exposed while you were investigating, revoke it at your provider first.

## What to expect

The maintainer is one person, working on this in spare time.

- Acknowledgement within 7 days.
- A follow-up on whether the report is accepted, and a rough plan, once it has been
  assessed. No fixed fix deadline is promised.
- Coordinated disclosure: the details are published, with credit to the reporter
  unless you prefer otherwise, after a fix is released or after a timeline agreed
  with you.

## Scope

Curia is pre-alpha: most of the areas below do not exist yet, and this policy applies to
them as they land.

In scope:

- API key storage: the OS secure store (Windows Credential Manager, macOS Keychain,
  Linux libsecret), the session-only fallback on Linux without a keyring, and any
  path by which a key could be written to disk in plain text.
- Key leakage into logs, crash output, diagnostics, or the text users are asked to
  paste into tester reports.
- TLS validation: any case where Curia accepts an invalid certificate or sends
  credentials over an unprotected channel.
- Update and download integrity: how release artefacts are built, checksummed and
  attested.
- The mod export path: the mod's `debug_log` output and the app's parser, for
  example crafted log content that crashes or misleads the app.

Out of scope:

- Problems in the LLM provider you configure, including how it stores or uses your
  requests. Realm data goes to the endpoint you choose, under its terms.
- A machine that is already compromised, or an attacker with your user account.
- Vulnerabilities in Crusader Kings III, the Paradox launcher or Steam.
- Findings that need the user to launch CK3 in debug mode or use the console, which
  Curia never asks for.

## Verifying downloads

Checksums (`SHA256SUMS`) and build-provenance attestations (verifiable with
`gh attestation verify`) will be published with the first release. There is no
release to verify yet. Until then, do not trust any binary labelled as Curia that
did not come from a build you made yourself from this repository.
