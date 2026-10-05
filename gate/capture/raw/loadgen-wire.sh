#!/bin/sh
timeout 6 tcpdump -i lo -n -l -A -s0 "tcp dst port 4490 and tcp[tcpflags] & tcp-push != 0" > /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-cpp/gate/capture/raw/loadgen-wire.txt 2>/dev/null &
sleep 1
docker run --rm --name g3-wire --network host -v /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-cpp/gate/capture:/out campfire-bench-runner sh -c 'C=$(/out/bin/loadgen login --base http://127.0.0.1:4490 --email david@37signals.com --password secret123456 | python3 -c "import json,sys;print(json.load(sys.stdin)[\"cookie\"])"); /out/bin/loadgen http --base http://127.0.0.1:4490 --cookie "$C" --post-room 201306877 --csrf "" --conc 1 --requests 1 --duration 3 >/dev/null'
wait
