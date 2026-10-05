# Sends raw bytes to a TCP port and prints the raw reply. Usage: rawhttp.py PORT 'GET / ...\r\n\r\n'
import socket, sys
port = int(sys.argv[1])
req = sys.argv[2].encode().decode("unicode_escape").encode("latin-1")
s = socket.create_connection(("127.0.0.1", port))
s.sendall(req)
s.settimeout(1.5)
data = b""
try:
    while True:
        chunk = s.recv(65536)
        if not chunk:
            break
        data += chunk
except OSError:
    pass
sys.stdout.buffer.write(data)
