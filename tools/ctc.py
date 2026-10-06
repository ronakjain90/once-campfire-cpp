#!/usr/bin/env python3
"""ctc: the Campfire template compiler (plans/architecture.md section 7.1).

Turns each `.ct` file under a root directory into one C++ function, and writes one header that
declares all of the functions.

Usage: ctc.py --root DIR --out DIR [--header-name PATH] [--exclude PREFIX]... [--require-source-comment]

Output: <out>/<header name> (default views/templates.gen.hpp) and <out>/<relative path>.ct.cpp for each template.

Syntax:
  text                        copied as it is
  {{ expr }}                  write expr with HTML escape (a SafeHtml value is written as it is)
  {{= expr }}                 write expr, which must be SafeHtml, with no escape
  {% if c %} {% elif c %} {% else %} {% end %}
  {% for x : range %} {% end %}
  {% render path(args) %}     call a partial (`layouts/lightbox` is the file layouts/_lightbox.ct)
  {% cache key %} {% end %}   fragment cache block
  {% call f(args) %}          write `f(out, args);` (a helper that writes to the output)
  {% wrap f(args) |a| %} {% end %}
                              write `f(out, args, [&](Out& out2, auto& a) { body });`
  {% params T a, U b %}       the parameters of the function (consumes its own line)
  {% include path.hpp %}      add an #include to the header and the source (consumes its line)
  {# text #}                  comment; the first comment names the ERB source file
  Trim (Erubi, trim mode): a line that holds only a `{% %}` statement tag or a `{# #}` comment
                              loses its indentation and its newline. Text before or after the tag
                              on the same line stops the trim. `{%-` and `-%}` are the same as `{%`
                              and `%}` (Erubi gives `<%-` and `-%>` no effect on a statement).
  `{% call %}` `{% wrap %}` `{% render %}` stand for ERB `<%= %>`: no trim, as for `{{ }}`.
  `-}}` `-%}`                 remove the newline after an expression (ERB `<%= x -%>`)
  `{{- `                      remove the indentation before an expression (ctc only)

Names: the file `a/b/_c.ct` is the function `campfire::views::a::b::c`. A dot in a file name
becomes `_`. A name that is a C++ keyword gets a `_` suffix.
"""
import argparse
import bisect
import os
import re
import sys


class CtcError(Exception):
    pass


KEYWORDS = set("""alignas alignof and and_eq asm auto bitand bitor bool break case catch char
char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast
continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum
explicit export extern false float for friend goto if inline int long mutable namespace new
noexcept not not_eq nullptr operator or or_eq private protected public register
reinterpret_cast requires return short signed sizeof static static_assert static_cast struct
switch template this thread_local throw true try typedef typeid typename union unsigned using
virtual void volatile wchar_t while xor xor_eq""".split())


class Tok:
    __slots__ = ("kind", "text", "line")

    def __init__(self, kind, text, line):
        self.kind = kind  # text, out, safe, stmt, comment
        self.text = text
        self.line = line


def skip_literal(src, i):
    """Return the index after the C++ string or char literal that starts at src[i]."""
    quote = src[i]
    i += 1
    while i < len(src):
        if src[i] == "\\":
            i += 2
            continue
        if src[i] == quote:
            return i + 1
        i += 1
    return i


def find_close(src, i, opener):
    """Find the closer of a tag whose body starts at i. Return (body_end, after_close)."""
    depth = 0
    n = len(src)
    while i < n:
        c = src[i]
        if opener != "{#" and c in "\"'":
            i = skip_literal(src, i)
            continue
        if opener.startswith("{{"):
            if c == "{":
                depth += 1
            elif c == "}":
                if depth == 0 and src.startswith("}}", i):
                    return i, i + 2
                depth = max(depth - 1, 0)
        elif opener == "{%":
            if c == "%" and src.startswith("%}", i):
                return i, i + 2
        elif c == "#" and src.startswith("#}", i):
            return i, i + 2
        i += 1
    raise CtcError("unterminated tag")


OPEN = re.compile(r"\{\{=?|\{%|\{#")
DIRECTIVE = re.compile(r"(params|include)\b")
EXPR_TAG = re.compile(r"\s*(call|wrap|render)\b")
SPACE_ONLY = re.compile(r"[ \t]*")
RSPACE = re.compile(r"[ \t]*\r?\n")


def lex(src, path):
    newlines = [m.start() for m in re.finditer("\n", src)]

    def line_of(offset):
        return bisect.bisect_left(newlines, offset) + 1

    toks = []
    pos = 0
    n = len(src)
    is_bol = True  # Erubi: the previous tag ended its line (or no tag yet)
    while True:
        m = OPEN.search(src, pos)
        end = m.start() if m else n
        text = src[pos:end]
        if m is None:
            if text:
                toks.append(Tok("text", text, line_of(pos)))
            return toks
        opener = m.group(0)
        body_start = m.end()
        stmt_like = opener in ("{%", "{#")
        # `{%-` and `{#-` are the same as `{%` and `{#`: Erubi gives `<%-` no extra effect.
        if stmt_like and src.startswith("-", body_start):
            body_start += 1
        # `call`, `wrap` and `render` stand for ERB `<%= ... %>`: Erubi does not trim an expression.
        if opener == "{%" and EXPR_TAG.match(src, body_start):
            stmt_like = False
        # Erubi (lib/erubi.rb): the spaces before a statement tag or a comment are "lspace" when
        # the tag is the first thing on its line.
        lspace = None
        if stmt_like:
            if text == "":
                if is_bol:
                    lspace = ""
            elif text.endswith("\n"):
                lspace = ""
            else:
                cut = text.rfind("\n")
                if cut >= 0:
                    if SPACE_ONLY.fullmatch(text[cut + 1:]):
                        lspace = text[cut + 1:]
                        text = text[:cut + 1]
                elif is_bol and SPACE_ONLY.fullmatch(text):
                    lspace = text
                    text = ""
        elif opener == "{{" and src.startswith("-", body_start) \
                and src[body_start + 1:body_start + 2] in (" ", "\t", "\r", "\n"):
            # `{{- ` (ctc only): remove the indentation before the tag, if the tag starts its line.
            body_start += 1
            stripped = text.rstrip(" \t")
            chunk_starts_line = pos == 0 or src[pos - 1] == "\n"
            if stripped.endswith("\n") or (stripped == "" and chunk_starts_line):
                text = stripped
        if text:
            toks.append(Tok("text", text, line_of(pos)))
        try:
            body_end, after = find_close(src, body_start, opener)
        except CtcError as e:
            raise CtcError(f"{path}:{line_of(m.start())}: {e}")
        body = src[body_start:body_end]
        trim_right = body.endswith("-")
        if trim_right:
            body = body[:-1]
        body = body.strip()
        kind = {"{{=": "safe", "{{": "out", "{%": "stmt", "{#": "comment"}[opener]
        rm = RSPACE.match(src, after)
        rspace = rm.group(0) if rm else None
        is_bol = rspace is not None
        directive = kind == "stmt" and DIRECTIVE.match(body)
        if stmt_like:
            # Erubi: a tag with lspace and rspace drops both. A `params` or `include` line is not
            # part of the output.
            if rspace is not None:
                after += len(rspace)
            if not (lspace is not None and rspace is not None) and not directive:
                if lspace:
                    toks.append(Tok("text", lspace, line_of(m.start())))
                if rspace is not None:
                    pending_rspace = rspace
                else:
                    pending_rspace = None
            else:
                pending_rspace = None
        else:
            pending_rspace = None
            if trim_right and rspace is not None:
                after += len(rspace)  # `-}}`: Erubi drops the newline of an expression
        toks.append(Tok(kind, body, line_of(m.start())))
        if pending_rspace:
            toks.append(Tok("text", pending_rspace, line_of(m.start())))
        pos = after


# ---- parser ----------------------------------------------------------------------------------

class Template:
    def __init__(self):
        self.params = ""
        self.includes = []
        self.comment = None  # the first comment
        self.body = []


def first_word(text):
    m = re.match(r"\w+", text)
    return m.group(0) if m else ""


def parse(toks, path):
    """Build the node tree. A node is a tuple whose first item is its kind."""
    tpl = Template()
    root = tpl.body
    # Each stack entry: (kind, node-list that receives nodes, opener node data)
    stack = []
    cur = root

    def err(tok, msg):
        raise CtcError(f"{path}:{tok.line}: {msg}")

    for tok in toks:
        k = tok.kind
        if k == "text":
            cur.append(("text", tok.text))
        elif k == "out":
            cur.append(("out", tok.text, tok.line))
        elif k == "safe":
            cur.append(("safe", tok.text, tok.line))
        elif k == "comment":
            if tpl.comment is None:
                tpl.comment = tok.text
            cur.append(("comment", tok.text))
        else:
            word = first_word(tok.text)
            rest = tok.text[len(word):].strip()
            if word == "params":
                if stack or cur is not root:
                    err(tok, "`params` must be at the top level")
                tpl.params = rest
            elif word == "include":
                tpl.includes.append(rest)
            elif word == "if":
                branches = [[rest, []]]
                node = ("if", branches, tok.line)
                cur.append(node)
                stack.append(("if", cur, node))
                cur = branches[0][1]
            elif word in ("elif", "else"):
                if not stack or stack[-1][0] != "if":
                    err(tok, f"`{word}` without `if`")
                branches = stack[-1][2][1]
                if branches[-1][0] is None:
                    err(tok, f"`{word}` after `else`")
                branches.append([rest if word == "elif" else None, []])
                cur = branches[-1][1]
            elif word == "for":
                m = split_for(rest)
                if m is None:
                    err(tok, "`for` needs `decl : range`")
                node = ("for", m[0], m[1], [], tok.line)
                cur.append(node)
                stack.append(("for", cur, node))
                cur = node[3]
            elif word == "cache":
                node = ("cache", rest, [], tok.line)
                cur.append(node)
                stack.append(("cache", cur, node))
                cur = node[2]
            elif word == "wrap":
                m = re.search(r"\|([^|]*)\|\s*$", rest)
                block_params = ""
                if m:
                    block_params = m.group(1).strip()
                    rest = rest[:m.start()].strip()
                node = ("wrap", rest, block_params, [], tok.line)
                cur.append(node)
                stack.append(("wrap", cur, node))
                cur = node[3]
            elif word == "end":
                if not stack:
                    err(tok, "`end` without a block")
                cur = stack.pop()[1]
            elif word == "render":
                cur.append(("render", rest, tok.line))
            elif word == "call":
                cur.append(("call", rest, tok.line))
            else:
                err(tok, f"unknown tag `{word}`")
    if stack:
        raise CtcError(f"{path}: `{stack[-1][0]}` is not closed")
    return tpl


def split_for(text):
    """Split `decl : range` at the first single colon outside brackets and literals."""
    depth = 0
    i = 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            i = skip_literal(text, i)
            continue
        if c in "([{<":
            depth += 1 if c != "<" else 0
        elif c in ")]}":
            depth -= 1
        elif c == ":" and depth == 0:
            if text.startswith("::", i):
                i += 2
                continue
            return text[:i].strip(), text[i + 1:].strip()
        i += 1
    return None


# ---- code generation ---------------------------------------------------------------------------

def ident(name):
    name = name.replace(".", "_").replace("-", "_")
    return name + "_" if name in KEYWORDS else name


def template_names(rel):
    """Return (namespace parts, function name) of a template file path without `.ct`."""
    parts = rel[:-3].split("/")
    func = parts[-1]
    if func.startswith("_"):
        func = func[1:]
    if not func:
        raise CtcError(f"{rel}: empty template name")
    return [ident(p) for p in parts[:-1]], ident(func)


def cpp_string(text):
    out = []
    for c in text:
        ch = ord(c)
        if c == "\\":
            out.append("\\\\")
        elif c == '"':
            out.append('\\"')
        elif c == "\n":
            out.append("\\n")
        elif c == "\r":
            out.append("\\r")
        elif c == "\t":
            out.append("\\t")
        elif ch >= 128 and ch not in (0x2028, 0x2029, 0x85):
            out.append(c)  # UTF-8 text stays readable in the source
        elif 32 <= ch < 127:
            out.append(c)
        else:
            out.append("\\u%04x" % ch if ch >= 128 else "\\%03o" % ch)
    return "".join(out)


class Gen:
    def __init__(self, rel):
        self.rel = rel
        self.lines = []
        self.ind = 1
        self.depth = 0  # nesting of lambdas; names the output variable

    def out(self):
        return "out" if self.depth == 0 else f"out{self.depth}"

    def add(self, text):
        self.lines.append("  " * self.ind + text)

    def line(self, n):
        self.lines.append(f'#line {n} "{self.rel}"')

    def text(self, s):
        pieces = re.findall(r"[^\n]*\n|[^\n]+", s)
        lits = ['"' + cpp_string(p) + '"' for p in pieces]
        head = f"{self.out()}.append(SafeHtml::literal("
        pad = "  " * self.ind + " " * len(head)
        for i, lit in enumerate(lits):
            prefix = "  " * self.ind + head if i == 0 else pad
            self.lines.append(prefix + lit + ("));" if i == len(lits) - 1 else ""))

    def body(self, nodes):
        for node in nodes:
            getattr(self, "node_" + node[0])(*node[1:])

    def node_text(self, s):
        self.text(s)

    def node_comment(self, s):
        if "\n" not in s and "*/" not in s:
            self.add(f"// {s}")

    def node_out(self, expr, line):
        self.line(line)
        self.add(f"::campfire::views::write({self.out()}, {expr});")

    def node_safe(self, expr, line):
        self.line(line)
        self.add(f"::campfire::views::write_safe({self.out()}, {expr});")

    def node_if(self, branches, line):
        self.line(line)
        for i, (cond, nodes) in enumerate(branches):
            if i == 0:
                self.add(f"if ({cond}) {{")
            elif cond is None:
                self.add("} else {")
            else:
                self.add(f"}} else if ({cond}) {{")
            self.ind += 1
            self.body(nodes)
            self.ind -= 1
        self.add("}")

    def node_for(self, decl, rng, nodes, line):
        if decl.startswith("["):
            decl = "const auto& " + decl
        elif not re.search(r"[\s&*]", decl):
            decl = "const auto& " + decl
        self.line(line)
        self.add(f"for ({decl} : {rng}) {{")
        self.ind += 1
        self.body(nodes)
        self.ind -= 1
        self.add("}")

    def node_render(self, call, line):
        m = re.match(r"^([\w/]+)\s*(?:\((.*)\))?\s*$", call, re.S)
        if not m:
            raise CtcError(f"{self.rel}:{line}: bad `render`: {call}")
        path, args = m.group(1), m.group(2)
        parts = [ident(p[1:] if p.startswith("_") and i == len(path.split("/")) - 1 else p)
                 for i, p in enumerate(path.split("/"))]
        name = "::".join(parts)
        if "/" in path:
            name = "::campfire::views::" + name
        self.line(line)
        self.add(f"{name}({self.out()}" + (f", {args}" if args and args.strip() else "") + ");")

    def helper_call(self, call, line, tail):
        m = re.match(r"^(.*?)\((.*)\)\s*$", call, re.S)
        if not m:
            raise CtcError(f"{self.rel}:{line}: bad call: {call}")
        args = m.group(2).strip()
        return f"{m.group(1)}({self.out()}" + (f", {args}" if args else "") + tail

    def node_call(self, call, line):
        self.line(line)
        self.add(self.helper_call(call, line, ");"))

    def lambda_block(self, head, params, nodes):
        self.depth += 1
        extra = f", {params}" if params else ""
        self.add(f"{head}[&](Out& {self.out()}{extra}) {{")
        self.ind += 1
        self.body(nodes)
        self.ind -= 1
        self.depth -= 1
        self.add("});")

    def node_wrap(self, call, params, nodes, line):
        self.line(line)
        m = re.match(r"^(.*?)\((.*)\)\s*$", call, re.S)
        if not m:
            raise CtcError(f"{self.rel}:{line}: bad `wrap`: {call}")
        args = m.group(2).strip()
        head = f"{m.group(1)}({self.out()}" + (f", {args}" if args else "") + ", "
        self.lambda_block(head, ("auto& " + params) if params else "", nodes)

    def node_cache(self, key, nodes, line):
        self.line(line)
        self.lambda_block(f"::campfire::views::cached({self.out()}, {key}, ", "", nodes)


def split_params(text):
    """Split a parameter list at the commas that are not inside brackets or literals."""
    parts, depth, start, i = [], 0, 0, 0
    while i < len(text):
        c = text[i]
        if c in "\"'":
            i = skip_literal(text, i)
            continue
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(text[start:i].strip())
            start = i + 1
        i += 1
    last = text[start:].strip()
    if last:
        parts.append(last)
    return parts


def generate(rel, tpl, header_name):
    """Return (source text, declaration lines) of one template."""
    ns, func = template_names(rel)
    gen = Gen(rel)
    gen.body(tpl.body)
    # A template does not have to use each parameter.
    plist = ["[[maybe_unused]] " + p for p in split_params(tpl.params)]
    params = "".join(", " + p for p in plist)
    sig = f"void {func}(Out& out{params})"
    src = [f"// Generated by tools/ctc.py from {rel}. Do not edit."]
    if tpl.comment:
        src.append(f"// Source: {tpl.comment.splitlines()[0].strip()}")
    src += ['#include "views/prelude.hpp"', f'#include "{header_name}"']
    src += [f"#include {inc}" if inc.startswith(("<", '"')) else f'#include "{inc}"' for inc in tpl.includes]
    src.append("")
    nsline = "namespace " + "::".join(["campfire", "views"] + ns) + " {"
    src += [nsline, "", sig + " {"]
    src.append("  using namespace ::campfire::views::helpers;  // NOLINT")
    src += gen.lines
    src += ["}", "", "}  // namespace " + "::".join(["campfire", "views"] + ns), ""]
    return "\n".join(src), (ns, func, sig + ";", tpl.includes)


def collect(root, excluded=()):
    found = []
    for dirpath, _, files in os.walk(root):
        for f in files:
            if f.endswith(".ct"):
                rel = os.path.relpath(os.path.join(dirpath, f), root).replace(os.sep, "/")
                if not rel.startswith(tuple(excluded)):
                    found.append(rel)
    return sorted(found)


def write_if_changed(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(path, encoding="utf-8") as f:
            if f.read() == text:
                return
    except FileNotFoundError:
        pass
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def header(decls):
    inc = sorted({i for d in decls for i in d[3]})
    lines = ["// Generated by tools/ctc.py. Do not edit.", "#pragma once", "",
             '#include "core/out.hpp"', '#include "core/html.hpp"']
    lines += [f"#include {i}" if i.startswith(("<", '"')) else f'#include "{i}"' for i in inc]
    lines.append("")
    by_ns = {}
    for ns, func, decl, _ in decls:
        by_ns.setdefault(tuple(ns), []).append(decl)
    for ns, items in sorted(by_ns.items()):
        full = "::".join(["campfire", "views"] + list(ns))
        lines.append(f"namespace {full} {{")
        lines += items
        lines.append(f"}}  // namespace {full}")
        lines.append("")
    return "\n".join(lines)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--header-name", default="views/templates.gen.hpp",
                    help="path of the generated header, below --out and in the #include lines")
    ap.add_argument("--exclude", action="append", default=[], metavar="PREFIX",
                    help="skip the templates whose relative path starts with PREFIX")
    ap.add_argument("--require-source-comment", action="store_true",
                    help="fail if a template has no comment that names its ERB source")
    args = ap.parse_args(argv)
    decls = []
    seen = {}
    outputs = []
    try:
        for rel in collect(args.root, args.exclude):
            with open(os.path.join(args.root, rel), encoding="utf-8") as f:
                src = f.read()
            tpl = parse(lex(src, rel), rel)
            if args.require_source_comment and not (tpl.comment and ".erb" in tpl.comment):
                raise CtcError(f"{rel}: the first comment must name the ERB source file")
            text, decl = generate(rel, tpl, args.header_name)
            key = (tuple(decl[0]), decl[1])
            if key in seen:
                raise CtcError(f"{rel}: same function name as {seen[key]}")
            seen[key] = rel
            decls.append(decl)
            outputs.append((os.path.join(args.out, rel + ".cpp"), text))
    except CtcError as e:
        print(f"ctc: error: {e}", file=sys.stderr)
        return 1
    for path, text in outputs:
        write_if_changed(path, text)
    write_if_changed(os.path.join(args.out, args.header_name), header(decls))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
