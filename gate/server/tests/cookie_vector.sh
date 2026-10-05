#!/opt/homebrew/bin/bash
# The cookie vector test: runs the gate's cookie code against once-campfire-rust/vectors/campfire_sessions.json.
HERE=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
. "$HERE/lib.sh"
SECRET=$(grep '^SECRET_KEY_BASE=' "$ENVF" | cut -d= -f2)
docker run --rm -v "$WORKSPACE/once-campfire-rust/vectors:/v:ro" --entrypoint /app/cookie_test campfire-gate:app /v/campfire_sessions.json "$SECRET"
