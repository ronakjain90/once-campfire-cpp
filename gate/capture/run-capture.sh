#!/usr/bin/env bash
# Run on the macOS host. Fresh app, tcpdump on port 9 in the VM, capture.py in a container, collect logs.
set -euo pipefail
W=/Volumes/ExternalHD/Code/AI/once-campfire
C=$W/once-campfire-cpp/gate/capture
cd "$C"; rm -rf raw; mkdir raw
./setup-app.sh
cat > raw/tcpdump.sh <<EOT
#!/bin/sh
exec tcpdump -i any -n -tt -l "tcp port 9" > $C/raw/tcpdump-port9.txt 2> $C/raw/tcpdump.err
EOT
chmod +x raw/tcpdump.sh
colima ssh -- sudo pkill tcpdump || true
colima ssh -- sudo sh -c "nohup $C/raw/tcpdump.sh >/dev/null 2>&1 </dev/null &"
sleep 2
docker run --rm --name g3-capture --network host -v "$C":/out -v $W/once-campfire-rust/parity/.seed/default:/seed:ro \
  campfire-bench-runner python3 /out/capture.py | tee raw/capture-summary.json
sleep 2
colima ssh -- sudo pkill tcpdump || true
docker logs -t g3-app > raw/app.log 2>&1
# Table state after the posts, from a read-only copy of the database file.
colima ssh -- sudo sh -c "mkdir -p /var/lib/campfire-bench/g3-snap && rm -f /var/lib/campfire-bench/g3-snap/* && cp /var/lib/campfire-bench/g3/db/production.sqlite3* /var/lib/campfire-bench/g3-snap/"
