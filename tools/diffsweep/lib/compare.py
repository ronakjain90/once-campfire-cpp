"""Comparison of two responses (or two Cable frame lists) and the report of the first difference."""
import difflib
import re

# Header values that differ on every response. Names and order are still compared.
DEFAULT_IGNORED_VALUES = {"date", "x-request-id", "x-runtime"}
# Cookies whose value is random per sign-in. The name, the value length and the attributes are compared.
DEFAULT_RANDOM_COOKIES = {"session_token"}

# Body normalizers: (name, regex, replacement). Only values that are random by design.
DEFAULT_NORMALIZERS = [
    ("csrf-meta", r'(<meta name="csrf-token" content=")[^"]*(")', r"\1<csrf>\2"),
    ("csrf-form", r'(name="authenticity_token"[^>]*? value=")[^"]*(")', r"\1<csrf>\2"),
    ("csrf-form-rev", r'(value=")[^"]*("[^>]*? name="authenticity_token")', r"\1<csrf>\2"),
]
TEXT_TYPE = re.compile(r"(text/|json|javascript|xml|turbo-stream)", re.I)


def normalize(data, extra=(), content_type="", use_defaults=True):
    """Apply the normalizers to bytes. Returns (bytes, list of names that changed something)."""
    if content_type and not TEXT_TYPE.search(content_type):
        return data, []
    rules = (list(DEFAULT_NORMALIZERS) if use_defaults else []) + [
        (n.get("name", "custom"), n["pattern"], n.get("replace", "<norm>")) for n in extra]
    used = []
    for name, pattern, repl in rules:
        data, n = re.subn(pattern.encode(), repl.encode(), data)
        if n:
            used.append(name)
    return data, used


def cookie_view(value, random_cookies):
    name, _, rest = value.partition("=")
    parts = [p.strip() for p in rest.split(";")]
    if name in random_cookies and parts[0]:
        parts[0] = f"<random len={len(parts[0])}>"
    return name + "=" + "; ".join(parts)


def header_lines(resp, ignored, random_cookies):
    out = []
    for name, value in resp.headers:
        if name == "set-cookie":
            value = cookie_view(value, random_cookies)
        elif name in ignored:
            value = "<ignored>"
        out.append(f"{name}: {value}")
    return out


def context(a, b, width=60):
    """The first differing byte offset and the bytes around it on each side."""
    n = min(len(a), len(b))
    i = next((k for k in range(n) if a[k] != b[k]), n)
    lo = max(0, i - width)
    show = lambda x: x[lo:i + width].decode("utf-8", "replace").replace("\n", "\\n")
    return i, show(a), show(b), lo


def text_diff(a, b, max_lines=40, max_width=220):
    al = a.decode("utf-8", "replace").splitlines()
    bl = b.decode("utf-8", "replace").splitlines()
    lines = list(difflib.unified_diff(al, bl, "expected", "actual", lineterm="", n=2))
    out = []
    for ln in lines[:max_lines]:
        out.append(ln if len(ln) <= max_width else ln[:max_width] + f"...(+{len(ln) - max_width} chars)")
    if len(lines) > max_lines:
        out.append(f"... ({len(lines) - max_lines} more diff lines)")
    return "\n".join(out)


def body_report(a, b):
    i, ca, cb, lo = context(a, b)
    rep = [f"first differing byte at offset {i} (expected {len(a)} bytes, actual {len(b)} bytes)",
           f"  expected[{lo}..]: {ca!r}", f"  actual  [{lo}..]: {cb!r}"]
    d = text_diff(a, b)
    if d:
        rep.append(d)
    return "\n".join(rep)


def compare_responses(exp, act, opts):
    """opts: ignored (set of header names), random_cookies, normalizers (list), defaults (bool).
    Returns (problems, report) where problems is a list of short strings and report is the text."""
    problems, parts = [], []
    for side, r in (("expected", exp), ("actual", act)):
        if r.error:
            problems.append(f"{side} request failed")
            parts.append(f"{side} request failed: {r.error}")
        if r.decode_error:
            problems.append(f"{side} body decode failed")
            parts.append(f"{side} body decode failed: {r.decode_error}")
    if any(r.error for r in (exp, act)):
        return problems, "\n".join(parts)
    if exp.status != act.status:
        problems.append("status")
        parts.append(f"status: expected {exp.status}, actual {act.status}")
    ignored = DEFAULT_IGNORED_VALUES | set(opts.get("ignored", ()))
    rc = DEFAULT_RANDOM_COOKIES | set(opts.get("random_cookies", ()))
    en, an = [k for k, _ in exp.headers], [k for k, _ in act.headers]
    eb, nb = normalize(exp.body, opts.get("normalizers", ()), exp.get("content-type") or "", opts.get("defaults", True))
    ab, na = normalize(act.body, opts.get("normalizers", ()), act.get("content-type") or "", opts.get("defaults", True))
    # A body with a random token also has a random ETag (a digest of the body).
    if "etag" not in ignored and (nb or na):
        ignored = ignored | {"etag"}
    eh, ah = header_lines(exp, ignored, rc), header_lines(act, ignored, rc)
    if en != an:
        problems.append("header names/order")
    if eh != ah:
        if en == an:
            problems.append("header values")
        parts.append("headers:\n" + "\n".join(difflib.unified_diff(eh, ah, "expected", "actual", lineterm="", n=1)))
    if eb != ab:
        problems.append("body")
        parts.append("body (decoded" + (", normalized" if nb or na else "") + "): " + body_report(eb, ab))
    return problems, "\n".join(parts)


def compare_frames(exp, act, opts, sort=False):
    """Compare two lists of Cable frame strings (after normalizing)."""
    def norm(frames):
        out = []
        for f in frames:
            data, _ = normalize(f.encode(), opts.get("normalizers", ()), "text/plain", opts.get("defaults", True))
            out.append(data.decode("utf-8", "replace"))
        return sorted(out) if sort else out
    e, a = norm(exp), norm(act)
    if e == a:
        return [], ""
    problems = ["frame count"] if len(e) != len(a) else ["frames"]
    parts = [f"frames: expected {len(e)}, actual {len(a)}"]
    for i in range(max(len(e), len(a))):
        x = e[i] if i < len(e) else None
        y = a[i] if i < len(a) else None
        if x != y:
            parts.append(f"frame {i} differs:")
            if x is None or y is None:
                parts.append(f"  expected: {x!r}\n  actual:   {y!r}"[:1500])
            else:
                parts.append(body_report(x.encode(), y.encode()))
            break
    return problems, "\n".join(parts)
