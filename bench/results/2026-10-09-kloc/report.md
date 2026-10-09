# Backend KLOC, 2026-10-09

The count follows the method of `docs/performance-review.md` of the shared verification harness.
KLOC is 1,000 nonblank, noncomment lines of backend application code.

## Result

| Implementation | Counted source revision | Files | Code lines | Backend KLOC |
|---|---|---:|---:|---:|
| C++ | `90b1d92` | 432 | 43,914 | 43.9 |

| Language | Files | Code lines |
|---|---:|---:|
| C++ | 218 | 34,774 |
| C/C++ Header | 214 | 9,140 |

## Method

- Tool: [cloc 2.10](https://github.com/AlDanial/cloc/tree/v2.10) with
  `--by-file --skip-uniqueness`. The SHA-256 of `cloc-2.10.pl` is
  `bf59272455172108072a0a106379f7509fd4349bdcfd85203bac038ccd286d83`.
- Included: the `.cpp` and `.hpp` files of `src/` that git tracks. This includes the HTTP server, the
  front server and the core library. The port wrote them, as the C fork wrote its own. Rendering
  helpers in C++ (`src/views/**/*.cpp`, `*.hpp`) are included.
- Excluded:
  - tests: `tests/` and `test/` folders.
  - fuzz targets: `fuzz/` folders.
  - benchmark tools: `bench/` folders.
  - build tools: `tools/` and `gen/` folders.
  - vendored code: `src/richtext/vendor/` (Gumbo, C).
  - templates: `.ct` files (the ERB views of the port).
  - route tables: `src/app/routes/*.inc`.
  - generated files: `src/storage/marcel_tables.cpp`, `src/db/schema.gen.hpp`,
    `src/models/search_words.cpp` and `src/views/sessions/translations_table.hpp`.

`files.txt` is the list of counted files. `cloc-by-file.txt` is the output of cloc.

To count again, run this from the repo root:

```sh
git ls-files src | grep -E '\.(cpp|hpp|c|h)$' \
  | grep -v -E '/(tests|test|fuzz|bench|vendor|tools|gen)/' \
  | grep -v -x -E 'src/storage/marcel_tables.cpp|src/db/schema.gen.hpp|src/models/search_words.cpp|src/views/sessions/translations_table.hpp' \
  > files.txt
perl cloc-2.10.pl --skip-uniqueness --list-file=files.txt
```
