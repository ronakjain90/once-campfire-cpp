# A2b: The room page, the messages page, the refresh and the message partials

The first part of A2 (merged from the Claude cloud session) built the sidebar, the room forms, direct rooms and involvements. This task finishes A2. Read `plans/specs/A2.md` first: its rules, split with A3 and acceptance still apply.

Owns: the files of this area in `src/app/`, `src/models/`, `src/views/` (extend A2's files; do not edit files of A3 or A4).

## What is missing (diff sweep `--area A2` on `main`: 97 of 150 requests differ)

- 81 requests give 404: `GET /rooms/:id`, `GET /rooms/:id/@:message_id` (and its redirects), `GET /rooms/:id/refresh`, `GET /rooms/:id/messages` (paging), with HEAD, 304 revisits and 406 cases.
- 16 requests are 200 on both sides with a different body, mostly the room edit and new forms. The "Go Back" link is the `last_room` cookie, which the missing room page sets, so most of these should go away with the room page. Fix any that remain.
- The A1 area still has 1 difference and the A8 area 14: all are room pages.

## Scope

- The message display partials (`messages/_message` and what it uses) with the fragment cache, as A2.md says. A3 is merged and calls them through `src/app/message_partial.hpp` with a marked stub in `src/app/message_partial_stub.cpp` (partials `message`, `presentation`, `attachment_presentation` in `views::messages`, and `boosts`, `boost` in `views::messages::boosts`). Write the real partials as `.ct` templates with those names, delete the stub, and point A3's code (`message_actions.cpp`, the controllers) at them. Merge A3's `message_presenter.cpp` and `sound.cpp` with anything you need rather than writing a second presenter. After this, the A3 sweep (`--area A3`) must lose all its kind (a) differences (28 now: bodies with a rendered message).
- Room show (with `@:message_id`), the messages page (paging), the refresh, and the `last_room` cookie.
- Performance: these are benchmark routes. Page cache with all inputs in the key; no render on a hit.

## Acceptance (raw output in the report)

- `bin/dev build` and `bin/dev test` for release, asan, tsan pass, including the page cache audit.
- Image `campfire-cpp:a2b`. Diff sweep against `campfire-rust:app`: `--area A2` 0 differences except request bodies that contain partials of A3 or A4 (list each), `--area A1` 0, `--area A8` 0, `--area A3` 0 except routes of A6 (the blob redirect).
- Benchmark: `gate/bench/run --apps rust=campfire-rust:app,cpp=campfire-cpp:a2b --routes room_show,messages_page,sidebar,post_message --reps 2`. Report the table.
