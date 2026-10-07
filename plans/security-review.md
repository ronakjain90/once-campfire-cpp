# Security review

This review covers the C++ port of Campfire. It lists each finding with its severity, the place in
the code, and the fix or the reason that it is not a problem. The reviewer read the code and ran
the fuzz targets and the sanitizer images (see the report of the task for the numbers).

Severity: high means a remote user without an account can crash the server or use all of its
memory. Medium means a policy is weaker than the policy of the Rails app. Low means a limit is
missing but another limit stops the problem.

## Findings that the review fixed

| # | Severity | Place | Problem | Fix |
|---|---|---|---|---|
| 1 | High | `src/net/worker_conn.cpp`, `Worker::fill_body` | A client that closed the connection in the middle of a request body made the server read a null `arena` pointer. The server stopped. One TCP connection was enough. | Return when the connection is closed. Test: `server survives a client that closes the connection in the middle of a body` in `src/net/tests/server_test.cpp`. |
| 2 | High | `src/net/worker_h2.cpp`, `H2Session::on_data_chunk` | The server kept all DATA of an HTTP/2 stream, also after the body passed the limit. It opened the flow control window again. A client could use all memory with a stream that never ends. | Do not keep the body after it passes the limit. The reply (413) goes out at the end of the stream, as before. Test: `an HTTP/2 request body above the limit gets 413` in `src/net/tests/h2_test.cpp`. |
| 3 | High | `src/net/worker_ws.cpp`, `Worker::ws_start` | The `Wire` of the 101 reply used memory of the request arena after `release_request` reset the arena. ASan reports a heap-use-after-free on each WebSocket upgrade. A release build reads freed memory at the same place. | Destroy the `Wire` before `release_request`. The Cable verification task owns this file and has the same fix (`b961980` on `task/A7v`). Until that branch merges, `main` has this defect. The test `src/net/tests/ws_test.cpp` fails under ASan without the fix. |
| 4 | Medium | `src/app/network_guard.cpp`, `blocked_v6` | The guard let through IPv4 compatible addresses (`::a.b.c.d`), 6to4 (`2002::/16`), Teredo (`2001::/32`), `2001:2::/48`, site local (`fec0::/10`), the local use NAT64 range (`64:ff9b:1::/48`) and SIIT (`::ffff:0:0:0/96`). It did not block an IPv4 mapped address. The surfguard gem of the Rails app blocks all of them. | The IPv6 rules are now the rules of surfguard, in the same order. Test: `src/app/tests/network_guard_test.cpp`. |
| 5 | Low | `src/app/unfurl_http.cpp`, `exchange` | A reply with a status line that has no space made the client add an offset to `npos`. | The client returns "wrong status line", as `Net::HTTP` does. Test: `unfurl_raw_test.cpp`. |
| 6 | Low | `src/app/unfurl_http.cpp`, `read_framed` | A chunked reply with no line end filled the memory of the client until the deadline. | The chunk size line has a limit of 4096 bytes. Test: `unfurl_raw_test.cpp`. |
| 7 | Low | `src/net/front/acme_http.cpp` | The JSON reader of the ACME client used recursion with no depth limit. The client read an answer with no size limit. | Depth limit of 64, size limit of 8 MiB. New fuzz target `fuzz_net_acme_json`. Test in `src/net/tests/misc_test.cpp`. |

## Checked and not a problem

| Area | Place | Result |
|---|---|---|
| Cookie signature | `src/compat/message_verifier.cpp`, `extract_encoded` | The compare uses `CRYPTO_memcmp` after a length check. |
| Cookie purpose and expiry | `src/compat/metadata.cpp`, `extract` | A message with a `pur` field never matches the purpose-less read. The expiry check is `now >= exp`. This is the behavior of Rails. |
| Key derivation | `src/compat/secrets.cpp` | PBKDF2 with the salts of Rails, one time at start. Each cookie type has its own key. |
| Encrypted cookie | `src/compat/crypto.cpp` | AES-256-GCM. The code checks the tag, the IV size and the key size. The IV comes from `RAND_bytes`. |
| Marshal in cookies | `src/compat/cookies.cpp`, `load` | Cookies refuse Marshal data. Other signed messages load only strings from Marshal, after a valid HMAC. |
| Session cookie | `src/app/concerns.cpp`, `set_authentication_cookie` | Signed, `httponly`, `SameSite=Lax`. `finish.cpp` adds `secure` when `force_ssl` is on. |
| Login | `src/models/user.cpp`, `authenticated` | A missing account takes as long as a wrong password. The rate limit is 10 in 3 minutes for each IP address. |
| IP address of the client | `src/app/proxy.cpp` | A port of `ActionDispatch::RemoteIp` with the trusted proxy list of Rails. |
| CSRF | `src/app/concerns.cpp`, `verify_authenticity_token` | A port of the Origin and `Sec-Fetch-Site` check of Rails. Only a bot key is exempt. |
| Authorization of rooms and messages | `src/app/controllers/*.cpp` | Each controller scopes the room, the message or the push subscription as the Rails controller does (`Current.user.rooms.find_by`, `reachable_messages`, `memberships.find_by!`). `can_administer` checks the creator or the administrator. |
| Rich text | `src/richtext/sanitizer.cpp` | The fuzz target checks that the output holds only allowed tags, no `on*` attribute, no `javascript:` URL and no comment. A nesting of 200000 levels is safe. |
| Outbound policy, unfurl | `src/app/opengraph/opengraph.cpp` | The guard resolves the name one time, and the client connects to that address. Each redirect goes through the guard again. Limit of 10 redirects, 5 MB for the body, a deadline for the whole fetch, TLS 1.2 or newer with host name check. |
| Outbound policy, push | `src/app/web_push.cpp`, `models/push_subscription.cpp` | Only https, port 443 and five push service host names, then the guard. The client does not follow redirects. |
| Outbound policy, webhook | `src/app/webhook.cpp` | There is no guard. This is the behavior of Rails (`app/models/webhook.rb` says that only an administrator sets the URL). |
| ACME | `src/net/front/tls.cpp`, `acme_http.cpp` | The challenge reply comes from a map of tokens, not from files. The client verifies the certificate of the CA and the host name. The 301 redirect uses the host of the TLS domain list. |
| File serving | `src/app/file_server.cpp`, `storage/disk.cpp`, `app/controllers/active_storage.cpp` | The path comes from a blob key that a signed message gives (purpose `blob_key`, 5 minute expiry). A user cannot choose a path. The range code is the code of Rack, with the 100 range limit and the total size limit. |
| Direct upload | `app/controllers/active_storage.cpp`, `disk_update` | The code needs a session, a signed token, the declared content type and length, and the MD5 checksum. |
| Multipart | `src/req/multipart.cpp` | Limits for the total size, the header size, the part count, the file count and the text size. The file name keeps only the base name. `mkstemp` makes the temp files with mode 0600, and the destructor of `UploadedFile` removes them. |
| WebSocket | `src/cable/websocket.cpp`, `app/channels/endpoint.cpp` | Origin check as `allow_request_origin?`. A message is 1 MiB at most, also after inflate. |
| HTTP/1 parser | `src/net/parser.cpp` | A request with `Transfer-Encoding` and `Content-Length`, a second `Transfer-Encoding`, or two different `Content-Length` values gets 400. Limits for the head, the target and the header count. |
| SQL | `src/models`, `src/db` | All queries use bound parameters. The full text query puts each word in quotes. |

## Open points

| # | Severity | Place | Note |
|---|---|---|---|
| A | Low | `src/app/file_server.cpp`, `read_file_range`; `src/app/rq_response.cpp`, `send_file` | The server reads a whole file into memory on the worker thread. A large blob blocks the worker for a while. This is a performance point, not a memory safety point. |
| B | Low | `src/storage/disk.cpp`, `upload` | The upload truncates and writes the final file. A concurrent download can read a partial file. Write to a temp name and rename. |
| C | Low | `ServerOptions::max_request_body` | The default is no limit. The memory limit of 16 MiB for each body still applies. Many large bodies at the same time can use much memory (250 streams of HTTP/2 times 16 MiB). Set `MAX_REQUEST_BODY` in production. |
| D | Info | `src/net/front/acme_http.cpp`, `dechunk` | The function is private and has no fuzz target. The ACME server is a trusted CA over TLS. |
| E | Info | `src/richtext` | The sanitizer uses its own HTML parser. A parser difference with a browser can allow mutation XSS. The fuzz target cannot find this class of problem. Compare the output with a browser on the corpus of known mXSS payloads. |
| F | Info | `src/app/message_actions.cpp`, sidebar files, `src/db` writer | Another task owns these files. They are outside the scope of this review. |
