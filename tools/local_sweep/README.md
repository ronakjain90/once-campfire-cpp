# local_sweep

A fast diff sweep with no Docker and no Rails seed. It starts the Rust binary and the C++ binary on copies of one
small SQLite database, sends the same requests to both, and compares status, header names and order, header values and
body bytes. It is for the work on one area at a time. The Docker tool `tools/diffsweep` stays the acceptance test.

## Run

1. Build the Rust port: `cargo build -p campfire --target-dir /tmp/rustbuild` in a clone of `once-campfire-rust`.
   The crate `campfire_assets` reads the Rails app at `reference/` (a git submodule): link a clone of `once-campfire`
   there first.
2. Build the C++ app: `campfire` target of the `release` or any other preset.
3. `pip install bcrypt`, then:

       RUST_BIN=/tmp/rustbuild/debug/campfire CPP_BIN=/tmp/cfbuild/campfire python3 tools/local_sweep/sweep.py [text]

`text` runs only the requests whose name contains it. The tool prints `same` or `DIFF` for each request, and `ALL SAME`
at the end. The room routes (area A2) are the requests in `scenarios()` and `mutating()`.

The tool ignores `date`, `x-request-id`, `x-runtime`, `etag` and `set-cookie` values.
