# Bundled font: Gentium Book Plus 6.200

The chat window's typeface (FR-OVL-3: a bundled, redistributable font; no network fetch for assets).

| | |
|---|---|
| Name | Gentium Book Plus, Regular and Bold (`GentiumBookPlus-Regular.ttf`, `GentiumBookPlus-Bold.ttf`) |
| Version | 6.200 (released 1 February 2023) |
| Designer | Victor Gaultney (original design of Gentium), with the SIL designers and font engineers (see the release's `FONTLOG.txt`) |
| Publisher | SIL International |
| Source | <https://github.com/silnrsi/font-gentium/releases/tag/v6.200>, archive `GentiumPlus-6.200.zip` |
| Licence | SIL Open Font License 1.1 (`OFL.txt`, SPDX `OFL-1.1`), Reserved Font Names "Gentium" and "SIL" |

## Files and checksums (sha256)

| File | sha256 |
|---|---|
| `GentiumPlus-6.200.zip` (the release archive, not committed) | `9b21103b79961149b6508791572acb3b2fe7eb621474c57d5e4ee37e76d7b073` |
| `GentiumBookPlus-Regular.ttf` | `298d3e2d2cdf0460d27151de50e2a5764d4de4921f5a6fc254ed5aeda6890f1e` |
| `GentiumBookPlus-Bold.ttf` | `bad0c69e4452a2c34d66754ad56a0f0c12b043cb40c44216337439eeae500831` |
| `OFL.txt` | `d8d18e5a10011298fa500ee393deb76c69749dd60ff3e524388d9750ec82cf63` |

The three files are copied from the archive **unmodified** and the two fonts are embedded in the `curia`
executable **byte for byte**: at build time `cmake/EmbedFile.cmake` turns each TTF into a generated source file
holding its bytes (`app/CMakeLists.txt`), so the app reads no font file at run time. `OFL.txt` keeps SIL's CRLF
line endings (`.gitattributes` marks it binary so that git does not convert them). The Reserved Font Names only
bind modified versions: Curia ships the font unchanged under its own name. The licence text is also in the
generated third-party notices (`cmake/extra_notices.txt`, `cmake --build <dir> --target curia_notices`).

To check a checkout: `shasum -a 256 app/resources/fonts/*.ttf app/resources/fonts/OFL.txt` (macOS) or
`sha256sum app/resources/fonts/*.ttf app/resources/fonts/OFL.txt` (Linux) must print the values above.

## Coverage

Measured with fontTools on the two files:

- Basic Latin and Latin-1 Supplement: complete;
- Latin Extended-A: 127 of 128 (only the deprecated U+0149 is missing);
- Latin Extended-B: 208 of 208; Latin Extended Additional: 256 of 256;
- Greek and Coptic: 121 of 135 (the Coptic letters are missing);
- Cyrillic: 220 of 256 (only archaic letters are missing);
- the bullet U+2022 the chat view uses for list lines;
- no CJK, Arabic or other scripts.

`curia --smoke` checks every character of its canned conversation (names with diacritics, a name in Cyrillic,
the bullet) and of the window's own texts against both faces and fails if one is missing.

## Why this font

- CK3 names its characters and titles in Latin script with many diacritics (Old Norse, Old English, Slavic,
  Arabic and Turkic names in transliteration), which Latin Extended-A, Extended-B and Extended Additional cover;
- Cyrillic, for players who write their questions in Russian;
- a book face that fits a medieval game, and that stays readable at the window's small sizes (it was designed
  for long reading at text sizes);
- the OFL allows bundling it in an MIT-licensed program, with the licence shipped alongside.

The choice is proposed for the maintainer's review (owner decision of 2026-10-11: an OFL font with Latin Extended
coverage, the exact font proposed in the PR that adds it).

## Replacing it

1. Put the new Regular and Bold TTF (or OTF) files and their licence here, unmodified, and update this README
   (name, version, source, checksums, licence, coverage).
2. Change the file names in `app/CMakeLists.txt` (the `foreach(face ...)` that embeds them).
3. Replace the Gentium section of `cmake/extra_notices.txt` with the new licence.
4. Build and run `curia --smoke`: it fails if a character the window shows is missing from the new font.
