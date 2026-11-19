SMALL FOCUSED FIX: shelf titles show black-diamond .notdef glyphs where curly apostrophes should be (e.g. "Death’s End" from calibre metadata renders "Death[?]s End"). The built-in UI font lacks typographic punctuation coverage.

Step 1 — diagnose precisely: inspect the built-in UI font generation (scripts/generate-font-manifest.py, src/fontIds.h, lib/EpdFont, docs on font generation in .agents/rules/generated-files-cache.md) and determine which of U+2018 U+2019 U+201A U+201B U+201C U+201D U+2013 U+2014 U+2015 U+2026 U+00A0 U+00B0 are missing from the UI font face used by list screens.

Step 2 — fix, preferred order:
(a) If the font pipeline supports adding glyphs to the UI font, add exactly the missing punctuation codepoints above (small set, minimal flash cost — measure and report the flash delta). Follow the repo's font-generation rules; do not hand-edit generated binaries.
(b) If (a) is impractical in this codebase, normalize at Kobo feed parse time instead: map the codepoints above to ASCII equivalents (’ ' ‚ ‛ -> ', “ ” -> ", – — ― -> -, … -> ..., NBSP -> space) when storing title/author fields in KoboCatalog/KoboFeedParser, with a host test.

Prefer (a); do both only if some surfaces render catalog text without going through the parser.

Verify: ~/.pio-venv/bin/pio run -e default and -e picco compile; report flash size delta vs current (default env currently ~5,656k flash); ctest suite passes (cmake -S test -B build/test && cmake --build build/test && ctest --test-dir build/test). No git commit. Minimal diff.
