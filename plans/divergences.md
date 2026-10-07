# Deliberate differences from the Rust port

The C++ app copies the deliberate differences of the Rust port from Rails (the Rust `README.md`,
"Known differences"). This file lists the places where the C++ app differs from the Rust port
on purpose. The diff sweep (`tools/diffsweep/`) accepts each difference here, and no other.

| Difference | Reason | How the sweep accepts it |
|---|---|---|
| Compressed bodies (gzip, zstd) have different bytes and a different `content-length`. The decoded bodies are equal. | The C++ app uses libdeflate and zstd with its own settings. The Rust port uses zlib-rs (flate2). The compressed bytes depend on the encoder, and a client sees only the decoded body. Approved by the user on 2026-10-07. | `lib/compare.py`: a different `content-length` is accepted only when both responses have the same `content-encoding` and the decoded bodies are equal. |
