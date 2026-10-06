cd /Volumes/ExternalHD/Code/AI/once-campfire/wt/T5
HTTP_PORT=18081 TARGET_PORT=18082 /build/release/campfire server > /tmp/campfire.log 2>&1 &
sleep 1
python3 tests/net/compare_golden.py 18081
cat /tmp/campfire.log
python3 tools/rawhttp.py 18082 'GET /up HTTP/1.1\r\nHost: x\r\n\r\nGET /up HTTP/1.0\r\nHost: x\r\n\r\n' | head -50
kill %1; wait
