# Security and cache checks

Image `campfire-cpp:fix2` (`sha256:bb7600c31990…`), harness seed at `4d1da88`. 90 of 90 checks pass.

| Mode | Check | Result | Detail |
|---|---|---|---|
| page cache 32 MB | fetch metadata: same-origin with Origin is accepted | pass | status 200, stored 1 |
| page cache 32 MB | fetch metadata: same-site is accepted | pass | status 200, stored 1 |
| page cache 32 MB | fetch metadata: no Fetch Metadata over plain HTTP is accepted | pass | status 200, stored 1 |
| page cache 32 MB | fetch metadata: cross-site is rejected | pass | status 422, stored 0 |
| page cache 32 MB | fetch metadata: none is rejected | pass | status 422, stored 0 |
| page cache 32 MB | fetch metadata: invalid Sec-Fetch-Site is rejected | pass | status 422, stored 0 |
| page cache 32 MB | fetch metadata: Origin null is rejected | pass | status 422, stored 0 |
| page cache 32 MB | fetch metadata: foreign Origin is rejected | pass | status 422, stored 0 |
| page cache 32 MB | fetch metadata: foreign Origin, no Sec-Fetch-Site is rejected | pass | status 422, stored 0 |
| page cache 32 MB | legacy token: an old tab still posts | pass | status 200 |
| page cache 32 MB | legacy token: a token does not allow a cross-site post | pass | status 422 |
| page cache 32 MB | fetch metadata: a cross-site GET (a link) works | pass | status 200 |
| page cache 32 MB | signed blob: a valid signed id redirects | pass | status 302 |
| page cache 32 MB | signed blob: a changed signed id is not found | pass | status 404 |
| page cache 32 MB | direct upload: no session gets no upload | pass | status 401 |
| page cache 32 MB | session transfer: a forged token signs nobody in | pass | status 400 |
| page cache 32 MB | join code: a wrong code is not found | pass | status 404 |
| page cache 32 MB | avatar: a changed avatar token is not found | pass | status 404 |
| page cache 32 MB | bot key: a valid key posts without Fetch Metadata | pass | status 201 |
| page cache 32 MB | bot key: a wrong key is rejected | pass | status 302 |
| page cache 32 MB | gzip: Accept-Encoding gzip | pass | status 200, gzip True, same body True |
| page cache 32 MB | gzip: Accept-Encoding gzip, deflate, br | pass | status 200, gzip True, same body True |
| page cache 32 MB | gzip: Accept-Encoding gzip;q=0 | pass | status 200, gzip False, same body True |
| page cache 32 MB | gzip: Accept-Encoding gzip;q=0.000 | pass | status 200, gzip False, same body True |
| page cache 32 MB | gzip: Accept-Encoding identity | pass | status 200, gzip False, same body True |
| page cache 32 MB | gzip: Accept-Encoding gzip;q=0 after 17 other codings | pass | status 200, gzip False, same body True |
| page cache 32 MB | gzip: Accept-Encoding *;q=0, identity | pass | status 200, gzip False, same body True |
| page cache 32 MB | direct SQL: a body edit with no timestamp change shows on the room page | pass | status 200 |
| page cache 32 MB | direct SQL: the body edit shows on the permalink page | pass | status 200 |
| page cache 32 MB | conditional: the room page has an ETag | pass | no ETag |
| page cache 32 MB | conditional: an old ETag gets the new page, not 304 | pass | status 200 |
| page cache 32 MB | conditional: If-Modified-Since in the future gets the new page | pass | status 200 |
| page cache 32 MB | conditional: the current ETag gets 304 | pass | status 304 |
| page cache 32 MB | direct SQL: a creator edit shows on the room page | pass | creator shown 149087659 |
| page cache 32 MB | direct SQL: a boost edit shows on the room page | pass | status 200 |
| page cache 32 MB | direct SQL: a body edit shows in search results | pass | status 200 |
| page cache 32 MB | revoked session: a deleted session gets no page | pass | status 302 |
| page cache 32 MB | revoked membership: the room page is gone | pass | status 302 |
| page cache 32 MB | revoked membership: the messages page is gone | pass | status 404 |
| page cache 32 MB | revoked membership: search shows no message of the room | pass | status 200 |
| page cache 32 MB | revoked membership: a post is rejected | pass | status 406 |
| page cache 32 MB | deactivated user: the session gets no page | pass | status 302 |
| page cache 32 MB | deactivated user: sign in fails | pass | status 401 |
| page cache 32 MB | revoked bot key: the old key is rejected | pass | status 302 |
| page cache 0 MB | fetch metadata: same-origin with Origin is accepted | pass | status 200, stored 1 |
| page cache 0 MB | fetch metadata: same-site is accepted | pass | status 200, stored 1 |
| page cache 0 MB | fetch metadata: no Fetch Metadata over plain HTTP is accepted | pass | status 200, stored 1 |
| page cache 0 MB | fetch metadata: cross-site is rejected | pass | status 422, stored 0 |
| page cache 0 MB | fetch metadata: none is rejected | pass | status 422, stored 0 |
| page cache 0 MB | fetch metadata: invalid Sec-Fetch-Site is rejected | pass | status 422, stored 0 |
| page cache 0 MB | fetch metadata: Origin null is rejected | pass | status 422, stored 0 |
| page cache 0 MB | fetch metadata: foreign Origin is rejected | pass | status 422, stored 0 |
| page cache 0 MB | fetch metadata: foreign Origin, no Sec-Fetch-Site is rejected | pass | status 422, stored 0 |
| page cache 0 MB | legacy token: an old tab still posts | pass | status 200 |
| page cache 0 MB | legacy token: a token does not allow a cross-site post | pass | status 422 |
| page cache 0 MB | fetch metadata: a cross-site GET (a link) works | pass | status 200 |
| page cache 0 MB | signed blob: a valid signed id redirects | pass | status 302 |
| page cache 0 MB | signed blob: a changed signed id is not found | pass | status 404 |
| page cache 0 MB | direct upload: no session gets no upload | pass | status 401 |
| page cache 0 MB | session transfer: a forged token signs nobody in | pass | status 400 |
| page cache 0 MB | join code: a wrong code is not found | pass | status 404 |
| page cache 0 MB | avatar: a changed avatar token is not found | pass | status 404 |
| page cache 0 MB | bot key: a valid key posts without Fetch Metadata | pass | status 201 |
| page cache 0 MB | bot key: a wrong key is rejected | pass | status 302 |
| page cache 0 MB | gzip: Accept-Encoding gzip | pass | status 200, gzip True, same body True |
| page cache 0 MB | gzip: Accept-Encoding gzip, deflate, br | pass | status 200, gzip True, same body True |
| page cache 0 MB | gzip: Accept-Encoding gzip;q=0 | pass | status 200, gzip False, same body True |
| page cache 0 MB | gzip: Accept-Encoding gzip;q=0.000 | pass | status 200, gzip False, same body True |
| page cache 0 MB | gzip: Accept-Encoding identity | pass | status 200, gzip False, same body True |
| page cache 0 MB | gzip: Accept-Encoding gzip;q=0 after 17 other codings | pass | status 200, gzip False, same body True |
| page cache 0 MB | gzip: Accept-Encoding *;q=0, identity | pass | status 200, gzip False, same body True |
| page cache 0 MB | direct SQL: a body edit with no timestamp change shows on the room page | pass | status 200 |
| page cache 0 MB | direct SQL: the body edit shows on the permalink page | pass | status 200 |
| page cache 0 MB | conditional: the room page has an ETag | pass | no ETag |
| page cache 0 MB | conditional: an old ETag gets the new page, not 304 | pass | status 200 |
| page cache 0 MB | conditional: If-Modified-Since in the future gets the new page | pass | status 200 |
| page cache 0 MB | conditional: the current ETag gets 304 | pass | status 304 |
| page cache 0 MB | direct SQL: a creator edit shows on the room page | pass | creator shown 149087659 |
| page cache 0 MB | direct SQL: a boost edit shows on the room page | pass | status 200 |
| page cache 0 MB | direct SQL: a body edit shows in search results | pass | status 200 |
| page cache 0 MB | revoked session: a deleted session gets no page | pass | status 302 |
| page cache 0 MB | revoked membership: the room page is gone | pass | status 302 |
| page cache 0 MB | revoked membership: the messages page is gone | pass | status 404 |
| page cache 0 MB | revoked membership: search shows no message of the room | pass | status 200 |
| page cache 0 MB | revoked membership: a post is rejected | pass | status 406 |
| page cache 0 MB | deactivated user: the session gets no page | pass | status 302 |
| page cache 0 MB | deactivated user: sign in fails | pass | status 401 |
| page cache 0 MB | revoked bot key: the old key is rejected | pass | status 302 |
| install compatibility | install: a session from campfire-rust:current works in campfire-cpp:fix2 | pass | status 200 |
| install compatibility | install: a session from campfire-cpp:fix2 works in campfire-rust:current | pass | status 200 |
