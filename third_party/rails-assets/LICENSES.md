# Licenses

| Files | License | License file |
|---|---|---|
| `reference/app/**`, `reference/public/**` | MIT (37signals) | `reference/MIT-LICENSE` |
| `vendor/<gem>/**` (lexxy, stimulus-rails, turbo-rails, actioncable, actiontext, actionview, activestorage, action_text-trix) | MIT | The file in `vendor/<gem>/` (`MIT-LICENSE` or `LICENSE`) |
| `overrides/**` | MIT (the Rust port, same as the reference) | `reference/MIT-LICENSE` |
| `reference/vendor/javascript/highlight.js/`, `reference/vendor/javascript/languages/` | BSD-3-Clause (highlight.js) | None in the reference. Each file keeps its own header. |
| `reference/vendor/javascript/@rails--request.js` | MIT (@rails/request.js 0.0.8) | None in the reference. |

The reference app does not ship the license text for highlight.js and @rails/request.js.
Add the text from the upstream packages before a public release.
