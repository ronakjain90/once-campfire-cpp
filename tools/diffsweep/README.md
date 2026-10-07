# diffsweep

diffsweep sends the same requests to two Campfire apps and compares the responses.
The first app is the expected app (default `campfire-rust:app`).
The second app is the actual app (default `campfire-cpp:app`).
The tool exits with code 1 if any response differs.

## Run

    tools/diffsweep/diffsweep [options] [list files or directories]

The launcher builds the `campfire-diffsweep` image on first use. The image extends `campfire-bench-runner`.
It runs `sweep.py` with `--network host` and with access to the Docker socket.
The default input is the directory `lists/`.

| Option | Meaning |
|---|---|
| `--expected IMAGE`, `--actual IMAGE` | The two images. Any two images work. |
| `--area A3` | Run only the lists of this area. Repeat the option for more areas. |
| `--only TEXT` | Run only the scenarios whose name contains TEXT. |
| `--seed NAME` | Use this seed for every list. |
| `--port N` | Port of the expected app. The actual app uses N+1. |
| `--out FILE` | Write a JSON report. Use a path under the workspace. |
| `--keep-going-verbose` | Print passing requests too. |

Run one area:

    tools/diffsweep/diffsweep --area A3
    tools/diffsweep/diffsweep --area A3 --only boosts

Run your own list:

    tools/diffsweep/diffsweep mylist.json

## What the tool does

1. It copies the seed from `once-campfire-rust/parity/.seed/NAME` to `/var/lib/campfire-bench/diffsweep/` for each app.
   Build a missing seed with `parity/bin/seed build NAME` (GNU `realpath` and bash 4 are required on macOS).
2. It points push endpoints and webhook URLs at a closed local port.
3. It starts both apps with the variables of `parity/.env.reference` and `CAMPFIRE_FROZEN_TIME` set to `clock.now` of the seed.
   Images that contain libfaketime (the Rails image) also get `FAKETIME`.
4. It runs each scenario against both apps in lockstep.
   A scenario that changes state (`"mutates": true`) runs on a fresh pair of apps.
   Read-only scenarios share one pair for each seed.
   The tool starts a new pair after 8 sign-ins, because of the sign-in rate limit.
5. It compares the two responses and prints a report for each difference.

## Comparison

- Status code.
- Header names and header order.
- Header values. These values are ignored: `date`, `x-request-id`, `x-runtime`, and the value of `etag` when a body normalizer changed the body.
- Cookie values of `session_token` and `_campfire_session` (random by design). The name and the attributes are compared.
- Decoded bodies, byte for byte. The tool decodes gzip, deflate, br and zstd.
- Frames of Action Cable sessions, as text.
- On a difference, the report shows the first differing byte offset, the bytes around it, and a unified diff.

Normalizers replace values that are random by design: the CSRF tokens, UUID ids of pending messages,
Active Storage signed keys. They are in `lib/compare.py`.
A list can add more with `"normalize": [{"name": "...", "pattern": "regex", "replace": "text"}]`
at the file, scenario or step level. A list can also add `"ignore_headers": ["name"]` and `"random_cookies": ["name"]`.

## List format

A list file is JSON.

    {"area": "A3", "env": ["NAME=value"], "scenarios": [
      {"name": "boosts", "seed": "default", "mutates": true, "steps": [ ... ]}]}

### Variables

`{{label}}` expands in paths, headers, bodies and Cable identifiers.
The label comes from `labels.json` of the seed (for example `{{rooms.designers}}`, `{{emails.david}}`),
or from a captured variable. `{{name|urlencode}}` encodes the value.

### Steps

| Step | Fields |
|---|---|
| request (default) | `path`, `method`, `actor`, `headers`, `form`, `json`, `body`, `multipart`, `user_agent`, `accept`, `accept_encoding`, `cookies` (false: no cookie jar), `csrf` (false: send no token), `revisit` (repeat with `If-None-Match` and `If-Modified-Since`), `capture`, `name` |
| `{"op": "login", "actor": "david"}` | Sign in as the seed user through the form. The user must be a label of `emails.NAME`. |
| `{"op": "set", "vars": {...}}` | Set variables. |
| `cable_connect` | `id`, `actor`, `origin`, `path`, `headers`. Compares the handshake. |
| `cable_subscribe` | `id`, `identifier` (object or string). |
| `cable_perform` | `id`, `identifier`, `action`, `data`. |
| `cable_wait` | `id`, `count`, `timeout`, `settle` (seconds to wait for extra frames), `sorted`, `ignore_types`. Compares the frames. |
| `cable_close` | `id`. |

Each actor has its own cookie jar on each app. `actor` defaults to `anon`.
A POST gets the token of the page of the actor in `X-CSRF-Token` when the app emits one.
It also gets `Sec-Fetch-Site: same-origin` (the Rust app checks this header).
`multipart` is `{"fields": {...}, "files": [{"name", "filename", "content_type", "path" or "text"}]}`.

`capture` is `{"var": {"from": "body|status|header:NAME", "regex": "...(group)...", "json": "a.b.0", "unescape": true, "optional": true}}`.
The tool captures the value on each side separately. This lets ids differ between the two apps.
A failed step ends its scenario and counts as a difference.

## Lists

`gen_lists.py` creates all files in `lists/` (run `tools/diffsweep/diffsweep gen`). Edit the generators, not the JSON.

| File | Source |
|---|---|
| `screens-A*.json` | The path of every state of `parity/screens.yml` (GET, with the actor, user agent and headers of the state), grouped by seed and actor. |
| `shape-A*.json` | The request set of `reference-tools/http_shape/sweep.py`. |
| `writes-A*.json` | Write flows: sign in and out, rate limit, transfer, join, first run, rooms, messages, edit, delete, boosts, uploads, bot API, profile, account, users, bans, bots, push, search, unfurl. |
| `cable-A7.json` | Cable sessions for the 7 channels, rejections, typing, presence, broadcasts after writes. |

| Area | Content of the lists |
|---|---|
| A1 | Sign-in, sessions, transfers, first run, join, root. |
| A2 | Rooms (all types), involvements, refreshes, sidebar. |
| A3 | Messages, boosts, uploads, bot API, autocomplete. |
| A4 | Account, users, profiles, avatars, bots, join codes, logo, custom styles, push, QR codes. |
| A5 | Search. |
| A6 | PWA, `/up`, error pages, assets, Active Storage, unfurl. |
| A7 | Cable. |
| A8 | Encodings, conditional requests, odd paths and methods. Plain HTTP only. |
| A9 | One message and ban flow that triggers webhook and push jobs (the deliveries go to a closed port). |

## Limits

- The tool has no TLS, ACME or HTTP/2 checks (A8) and no check of the content of outgoing deliveries (A9).
- The Rust app emits no CSRF token. The tool compares the bodies as they are.
  Against Rails, every page differs by the two CSRF meta tags.
- `screens.yml` states with browser steps (clicks, typing) are covered as GETs and by the write flows. The tool has no browser.

## Sanitizer images and parallel runs

Two environment variables let more than one agent run the tool at the same time, and let it run a
sanitizer image as the actual app.

| Variable | Meaning |
|---|---|
| `DIFFSWEEP_PREFIX` | Prefix of the container names (default `diffsweep`). Use a different prefix and `--port` for each user. |
| `DIFFSWEEP_ACTUAL_ENV` | `NAME=value;NAME=value`: more environment for the actual app (for example `ASAN_OPTIONS`). |
| `DIFFSWEEP_SANITIZER_RUN` | Any value: start the actual app with `seccomp=unconfined`. A TSan image needs this. |
| `DIFFSWEEP_SAVE_LOGS` | A directory. The tool keeps the container log of each actual app there (sanitizer reports are in it). |
