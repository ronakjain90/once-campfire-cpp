#!/bin/sh
exec tcpdump -i any -n -tt -l "tcp port 9" > /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-cpp/gate/capture/raw/tcpdump-port9.txt 2> /Volumes/ExternalHD/Code/AI/once-campfire/once-campfire-cpp/gate/capture/raw/tcpdump.err
