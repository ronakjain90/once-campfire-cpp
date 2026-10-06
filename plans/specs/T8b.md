# T8b: Erubi trim rules in the template compiler

Owns: `tools/ctc.py`, `src/views/` (the templates and tests that the change affects).

Rails renders ERB with Erubi in trim mode. Erubi removes the leading spaces and the newline of a
line that holds only a statement tag (`<% ... %>`, not `<%= %>`). Today `ctc.py` trims only where a
template writes `-%}` or `{%-`. Make `ctc.py` apply Erubi's rule to `{% ... %}` statement tags by
itself, so that a `.ct` file can follow its ERB file line for line with no trim markers.

## Scope

1. Read the Erubi source in `campfire-reference:app` (`docker run --rm campfire-reference:app bundle show erubi`) and the Rails ActionView Erubi handler settings. Copy the exact rule, including the edge cases: a statement tag with text before or after it on the same line, `<%-` and `-%>`, `<%#` comments, and the last line of a file with no newline.
2. Implement the rule in `ctc.py` for statement tags (`if`, `elif`, `else`, `end`, `for`, `cache`, `call`, `wrap`, `render` when it is a statement, comments). Keep `-%}` and `{%-` working as Erubi's `-%>` and `<%-`.
3. Remove trim markers from the existing templates (`src/views/layouts/*`) where Erubi's rule now does the same work, so the templates follow their ERB files.
4. Add ERB comparison cases (made by running Erubi in `campfire-reference:app`) for each edge case.

## Acceptance (raw output in the report)

- `bin/dev build` and `bin/dev test` for release, asan, tsan pass.
- All existing golden and ERB comparisons still pass, plus the new edge cases (counts).
