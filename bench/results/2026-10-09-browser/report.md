# Browser checks of the shared verification harness, 2026-10-09

The run used `bin/browser` of
[basecamp/once-campfire-verification](https://github.com/basecamp/once-campfire-verification) at
revision `e244051`. The check drives Chromium through the flows that every port must pass, on a
fresh install of the app.

## Result

`90b1d92` passed 3 runs of 3. Each run used a new container with empty storage.

```
PASS: setup, two-tab live messaging, duplicate suppression, and stored-markup safety, copying
message permalinks, editing, search, profile/account updates, QR codes, live room
creation/renaming, bots, custom styles, session transfers, joining, and autocomplete-started
direct pings in Chromium.
```

`run-1.txt`, `run-2.txt` and `run-3.txt` are the outputs of the three runs.

## Before the fixes

`2aa6531` (`main` before the fixes) failed. The output of that run is in `before-fix-2aa6531.txt`.
Three causes stopped the check, one after the other:

| Step | Cause | Fix |
|---|---|---|
| Edit a message | The port had no `Rack::MethodOverride`. A form POST with `_method=patch` got 404. | `b56c749` |
| Session transfer | The transfer page did not close its form (Rails `27f5461`). | `3763b38` |
| Start a ping | A sidebar refresh replaced the ping editor (Rails `8bbe129`, Rust override). | `90b1d92` |

## Setup

| Item | Value |
|---|---|
| C++ source | `90b1d92`, clean |
| C++ image | `sha256:1bd1b825…`, from `docker/Dockerfile` |
| Harness | `e244051` |
| Browser | Playwright 1.63.0, Chromium 1243, headless |
| Node.js | 26.11.0 on macOS |
| Host | Apple M4 Mac. Docker runs in a Colima VM. |

The app ran with the disposable settings of the harness fixture (`fixtures/default/reference.env`)
and port 3100:

```sh
docker run -d -p 127.0.0.1:3100:80 --env-file fixtures/default/reference.env campfire-cpp:browser
bin/browser --base http://127.0.0.1:3100
```
