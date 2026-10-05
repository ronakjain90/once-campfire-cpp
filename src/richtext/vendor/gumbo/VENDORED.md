# Vendored Gumbo

Source: the libgumbo fork in the Nokogiri source gem, `gumbo-parser/src/` and `gumbo-parser/{CHANGES.md,THANKS}`.

- Nokogiri version: 1.19.4 (from `once-campfire-rust/reference/Gemfile.lock`).
- Gem URL: https://rubygems.org/downloads/nokogiri-1.19.4.gem
- SHA-256 of the gem file: 50c951611c92bca05c51411aef45f1cbc50f2821c4802758c5c6d34696533ab5
- SHA-256 of the list of per-file hashes of this directory (all files except this one, sorted by name, `shasum -a 256` of each, then `shasum -a 256` of that output): 2d08317b28eb5432c7e4394a213512ee7b292ce89239bd93aeb27c5ad6e80cb5
- One local patch in `parser.c` (search for "Campfire patch"): in a fragment parse, the attributes of a stray `<html>` tag are dropped instead of merged into the root html element. Nokogiri never reads that element's attributes, so the output is the same, and the merge was quadratic in the number of attributes. The per-file hashes above are of the original files; `parser.c` differs by this patch.
- All other files are unchanged. The gem's `gumbo-parser/Makefile` (test build) is not copied.
- License: Apache 2.0. Each source file has the notice. `LICENSE` holds the standard text (the gem has no separate copy; upstream is google/gumbo-parser COPYING).
- Nokogiri calls Gumbo with: max_attributes 400, max_errors 0, max_tree_depth 400 (401 for a fragment, for the html element), fragment context "body" in the HTML namespace, no-quirks mode.
