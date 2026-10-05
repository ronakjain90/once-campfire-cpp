"""HTTP client for the diff sweep: raw responses, header order kept, bodies decoded, cookie jars."""
import gzip
import http.client
import subprocess
import urllib.parse
import zlib

CHROME = ("Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
          "Chrome/140.0.0.0 Safari/537.36")
HTML = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"


def decode_body(raw, encodings):
    """Decode a body through the Content-Encoding list (applied last-first)."""
    data = raw
    for enc in reversed(encodings):
        enc = enc.strip().lower()
        if not enc or enc == "identity":
            continue
        if enc in ("gzip", "x-gzip"):
            data = gzip.decompress(data)
        elif enc == "deflate":
            try:
                data = zlib.decompress(data)
            except zlib.error:
                data = zlib.decompress(data, -15)
        elif enc == "br":
            import brotli
            data = brotli.decompress(data)
        elif enc == "zstd":
            p = subprocess.run(["zstd", "-dc"], input=data, capture_output=True)
            if p.returncode != 0:
                raise ValueError("zstd decode failed: " + p.stderr.decode("utf-8", "replace"))
            data = p.stdout
        else:
            raise ValueError("unknown content-encoding " + enc)
    return data


class Response:
    def __init__(self, status, headers, raw, error=None):
        self.status = status
        self.headers = headers  # list of (lowercase name, value), in wire order
        self.raw = raw
        self.error = error
        self.decode_error = None
        encs = [e for v in self.all("content-encoding") for e in v.split(",")]
        try:
            self.body = decode_body(raw, encs)
        except Exception as e:  # keep the raw bytes, report later
            self.body = raw
            self.decode_error = str(e)

    def all(self, name):
        return [v for k, v in self.headers if k == name]

    def get(self, name):
        v = self.all(name)
        return ", ".join(v) if v else None

    def text(self):
        return self.body.decode("utf-8", "replace")


class Client:
    """One actor on one app: a cookie jar and the CSRF token of its session."""

    def __init__(self, port, host_header, timeout=60):
        self.port = port
        self.host_header = host_header
        self.timeout = timeout
        self.cookies = {}
        self.csrf = None
        self.logged_in = False

    def cookie_header(self):
        return "; ".join(f"{k}={v}" for k, v in self.cookies.items())

    def request(self, method, path, headers=None, body=None, use_cookies=True):
        h = {"User-Agent": CHROME, "Accept": HTML, "Accept-Encoding": "gzip, deflate, br, zstd",
             "Host": self.host_header}
        h.update(headers or {})
        if use_cookies and self.cookies and "Cookie" not in h:
            h["Cookie"] = self.cookie_header()
        try:
            conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=self.timeout)
            conn.putrequest(method, path, skip_host=True, skip_accept_encoding=True)
            for k, v in h.items():
                if v != "":
                    conn.putheader(k, v)
            if body is not None:
                conn.putheader("Content-Length", str(len(body)))
            conn.endheaders(body)
            r = conn.getresponse()
            raw = r.read()
            headers_out = [(k.lower(), v) for k, v in r.getheaders()]
            conn.close()
        except Exception as e:
            return Response(0, [], b"", error=f"{type(e).__name__}: {e}")
        resp = Response(r.status, headers_out, raw)
        if use_cookies:
            self.absorb_cookies(resp)
        return resp

    def absorb_cookies(self, resp):
        for value in resp.all("set-cookie"):
            name, _, rest = value.partition("=")
            parts = [p.strip() for p in rest.split(";")]
            expired = any(p.lower().startswith("expires=") and "1970" in p for p in parts[1:])
            if parts[0] and not expired:
                self.cookies[name] = parts[0]
            else:
                self.cookies.pop(name, None)


def multipart(fields, files, boundary="----diffsweepboundary7MA4YWxkTrZu0gW"):
    """Build a multipart/form-data body. files: [{name, filename, content_type, data(bytes)}]."""
    out = []
    for k, v in fields.items():
        out.append(f'--{boundary}\r\nContent-Disposition: form-data; name="{k}"\r\n\r\n{v}\r\n'.encode())
    for f in files:
        out.append((f'--{boundary}\r\nContent-Disposition: form-data; name="{f["name"]}"; '
                    f'filename="{f["filename"]}"\r\nContent-Type: {f["content_type"]}\r\n\r\n').encode()
                   + f["data"] + b"\r\n")
    out.append(f"--{boundary}--\r\n".encode())
    return b"".join(out), f"multipart/form-data; boundary={boundary}"


def urlencode(form):
    pairs = []
    for k, v in form.items():
        for item in (v if isinstance(v, list) else [v]):
            pairs.append((k, item))
    return urllib.parse.urlencode(pairs).encode()
