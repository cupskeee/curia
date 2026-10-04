## Summary

<!-- What does this change and why? Keep it short. -->

## Linked issue

<!-- e.g. "Closes #123", or "None". -->

## Type

The PR title becomes the squash-merge commit message, so it must follow [Conventional Commits](https://www.conventionalcommits.org/en/v1.0.0/), for example `feat(app): add settings window`. Tick the type you used.

- [ ] `feat`: new user-visible capability
- [ ] `fix`: bug fix
- [ ] `docs`: documentation only
- [ ] `refactor`: code change with no behaviour change
- [ ] `perf`: performance improvement
- [ ] `test`: tests only
- [ ] `build`: build system or dependencies
- [ ] `ci`: CI configuration
- [ ] `chore`: anything else
- [ ] Breaking change (`!` in the title or a `BREAKING CHANGE:` footer)

## How it was tested

<!-- Commands you ran (for example `ctest --preset macos-arm64`) and what you checked by hand. -->

Tested on:

- [ ] macOS
- [ ] Windows
- [ ] Linux
- [ ] CI only (not run locally)

OS and version:

## Checklist

- [ ] The `CI result` check is green.
- [ ] `CHANGELOG.md` and the docs are updated for user-visible changes (or this change has none).
- [ ] No API keys, tokens or other secrets appear in code, logs, test fixtures or this description.
- [ ] No Paradox content (game script files, `script_docs` or `dump_data_types` output, raw `debug.log` captures) and no Voices of the Court code or script is included.
