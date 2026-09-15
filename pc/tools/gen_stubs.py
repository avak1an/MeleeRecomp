#!/usr/bin/env python3
"""Generate pc/generated/stubs.c: no-op implementations of Dolphin SDK
functions that the PC build does not implement yet.

The set of functions is data-driven: pass the MSVC link log (the LNK2019 /
LNK2001 lines name every unresolved symbol) and the generator looks each
name up in the SDK headers to reproduce its exact prototype. Names it
cannot find (data symbols, MSL libc functions) are reported so they can be
implemented by hand in pc/src.

    python pc/tools/gen_stubs.py --link-log build/pc/link.log
    python pc/tools/gen_stubs.py --symbols names.txt

Each stub records the call through pc_stub_hit(), so a run of the binary
ends with a list of which SDK entry points the game actually needed.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
INCLUDE_ROOT = REPO / "extern" / "dolphin" / "include"
OUTPUT = REPO / "pc" / "generated" / "stubs.c"
KEEP_FILE = REPO / "pc" / "generated" / "stubs.txt"

# Tokens that may decorate a return type and must not end up in the stub.
ATTRIBUTE_TOKENS = {
    "DOLPHIN_ATTRIBUTE_NORETURN",
    "ATTRIBUTE_NORETURN",
    "extern",
    "inline",
    "__inline",
    "static",
    "register",
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    return text


def strip_preprocessor(text: str) -> str:
    # Drop directives but keep the line structure irrelevant: we only need
    # statements terminated by ';'.
    return re.sub(r"^[ \t]*#[^\n]*(?:\\\n[^\n]*)*", " ", text, flags=re.M)


PROTO_RE = re.compile(
    r"^\s*(?P<ret>[A-Za-z_][\w\s\*]*?)\s*\b(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<params>[^()]*(?:\([^()]*\)[^()]*)*)\)\s*$",
    re.S,
)


def collect_prototypes(root: Path) -> dict[str, tuple[str, str, Path]]:
    """Map function name -> (return type, parameter list, header)."""
    protos: dict[str, tuple[str, str, Path]] = {}
    for header in sorted(root.rglob("*.h")):
        text = strip_preprocessor(strip_comments(header.read_text(encoding="utf-8", errors="replace")))
        # Remove function bodies so `static inline` helpers don't confuse the
        # statement splitter.
        # `extern "C" {` blocks must not count as bodies.
        text = re.sub(r'extern\s*"C"\s*\{', " ", text)
        depth = 0
        out = []
        for ch in text:
            if ch == "{":
                depth += 1
                continue
            if ch == "}":
                if depth == 0:
                    continue  # closer of an extern "C" block
                depth -= 1
                out.append(";")
                continue
            if depth == 0:
                out.append(ch)
        text = "".join(out)
        for stmt in text.split(";"):
            if "typedef" in stmt or "=" in stmt or "(*" in stmt:
                continue
            m = PROTO_RE.match(stmt)
            if not m:
                continue
            ret_tokens = [t for t in m.group("ret").replace("*", " * ").split() if t not in ATTRIBUTE_TOKENS]
            if not ret_tokens or ret_tokens[0] in {"return", "else", "goto"}:
                continue
            ret = " ".join(ret_tokens).replace(" *", "*")
            name = m.group("name")
            params = " ".join(m.group("params").split())
            if name in protos:
                continue
            protos[name] = (ret, params, header.relative_to(root))
    return protos


BASE_TYPE_TOKENS = {"const", "volatile", "struct", "enum", "union", "unsigned", "signed",
                    "long", "short", "int", "char", "float", "double", "void"}
BUILTIN_TOKENS = {"unsigned", "signed", "long", "short", "int", "char", "float", "double", "void"}


def name_parameters(params: str) -> str:
    """A prototype may leave parameters unnamed; a definition may not."""
    if params.strip() == "void":
        return params
    out = []
    for i, param in enumerate(params.split(",")):
        tokens = param.replace("*", " * ").split()
        idents = [t for t in tokens if t != "*" and t not in BASE_TYPE_TOKENS]
        named = len(idents) >= 2 or (len(idents) == 1 and any(t in BUILTIN_TOKENS for t in tokens))
        out.append(param.strip() if named else f"{param.strip()} _p{i}")
    return ", ".join(out)


def read_symbols_from_link_log(path: Path) -> set[str]:
    names = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r"unresolved external symbol _?(\w+)", line)
        if m:
            names.add(m.group(1))
    return names


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--link-log", type=Path, help="MSVC link output to harvest unresolved symbols from")
    ap.add_argument("--symbols", type=Path, help="text file with one symbol name per line")
    ap.add_argument("--add", action="store_true", help="add to the existing stub list instead of replacing it")
    ap.add_argument("--remove", nargs="*", default=[], help="names to drop from the stub list")
    ap.add_argument("--output", type=Path, default=OUTPUT)
    args = ap.parse_args()

    wanted: set[str] = set()
    if KEEP_FILE.exists() and (args.add or (not args.link_log and not args.symbols)):
        wanted |= {l.strip() for l in KEEP_FILE.read_text().splitlines() if l.strip()}
    if args.link_log:
        wanted |= read_symbols_from_link_log(args.link_log)
    if args.symbols:
        wanted |= {l.strip() for l in args.symbols.read_text().splitlines() if l.strip()}
    wanted -= set(args.remove)

    protos = collect_prototypes(INCLUDE_ROOT)
    found = sorted(n for n in wanted if n in protos)
    missing = sorted(n for n in wanted if n not in protos)

    headers = sorted({str(protos[n][2]).replace("\\", "/") for n in found})
    lines = [
        "/* Generated by pc/tools/gen_stubs.py. Do not edit; rerun the generator. */",
        "/* clang-format off */",
        '#include "pc_runtime.h"',
        "",
    ]
    lines += [f"#include <{h}>" for h in headers]
    lines += ["", "#define STUB(name) pc_stub_hit(#name)", ""]
    for name in found:
        ret, params, _ = protos[name]
        if not params:
            params = "void"
        params = name_parameters(params)
        lines.append(f"{ret} {name}({params})")
        lines.append("{")
        lines.append(f"    STUB({name});")
        if ret != "void":
            lines.append(f"    {{ static {ret} zero; return zero; }}")
        lines.append("}")
        lines.append("")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("\n".join(lines), encoding="utf-8", newline="\n")
    KEEP_FILE.write_text("\n".join(found) + "\n", encoding="utf-8", newline="\n")

    print(f"wrote {len(found)} stubs to {args.output.relative_to(REPO)}")
    if missing:
        print(f"{len(missing)} symbols not found in SDK headers (implement by hand):")
        for n in missing:
            print("  " + n)
    return 0


if __name__ == "__main__":
    sys.exit(main())
