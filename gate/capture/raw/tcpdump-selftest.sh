#!/bin/sh
timeout 4 tcpdump -i any -n -tt -l "tcp port 9" > /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-cpp/gate/capture/raw/tcpdump-selftest.txt 2>/dev/null &
sleep 1; curl -s -m 2 http://127.0.0.1:9/x; curl -s -m 2 http://127.0.0.1:9/y; wait
