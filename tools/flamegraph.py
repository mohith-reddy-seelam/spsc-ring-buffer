#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Symbolise raw stacks from spsc_profile and render a flame graph.

    python tools/flamegraph.py build/release/spsc_profile.exe stacks.txt out.svg \
        --title "SPSC ring: throughput"

Input lines (written by harness/sampler.cpp) look like

    producer;ntdll.dll!RtlUserThreadStart;...;exe+0x1a2b;exe+0x1c3d 1234

`exe+0x<rva>` frames are resolved with binutils addr2line, which reads the
DWARF debug info, including inlined frames, so `RingBuffer::try_push` shows up
even though it was inlined into the caller. Frames from other modules were
already resolved in-process. The folded output is the standard format consumed
by Brendan Gregg's flamegraph.pl (fetched into third_party/ on first use).
"""
from __future__ import annotations

import argparse
import bisect
import re
import shutil
import subprocess
import sys
from collections import Counter
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FLAMEGRAPH_DIR = ROOT / "third_party" / "FlameGraph"
FLAMEGRAPH_REPO = "https://github.com/brendangregg/FlameGraph.git"
# First frame kept under the thread name (see bench/profile_main.cpp).
ROOT_FRAME = re.compile(r"^(?:\S+ )?(producer|consumer)_(throughput|rtt)<")


def tool(name: str) -> str:
    path = shutil.which(name)
    if not path:
        sys.exit(f"error: '{name}' not found on PATH (it ships with binutils / MSYS2 / git)")
    return path


def image_base(exe: Path) -> int:
    out = subprocess.run([tool("objdump"), "-p", str(exe)], capture_output=True, text=True,
                         check=True).stdout
    m = re.search(r"^ImageBase\s+([0-9a-fA-F]+)", out, re.M)
    return int(m.group(1), 16) if m else 0


def simplify(name: str) -> str:
    """Drop parameter lists and noise so frames fit in a flame graph cell."""
    name = name.replace(";", ":")
    # c++filt cannot demangle some C++20 requires-clauses; keep the base name.
    m = re.match(r"_ZSt(\d+)", name)
    if m:
        n = int(m.group(1))
        return "std::" + name[m.end():m.end() + n]
    # Strip a trailing "(args)" (with nested parentheses), keep "operator()".
    depth, cut = 0, None
    for i in range(len(name) - 1, -1, -1):
        c = name[i]
        if c == ")":
            depth += 1
        elif c == "(":
            depth -= 1
            if depth == 0:
                cut = i
                break
    if cut is not None and not name[:cut].endswith("operator"):
        name = name[:cut]
    name = name.replace("spsc::Tuning{128ull, true, (spsc::Signal)1}", "default")
    name = re.sub(r"\bbench::Msg\b", "Msg", name)
    name = re.sub(r"std::__cxx11::", "std::", name)
    return name


class SymbolTable:
    """Fallback for code without DWARF (the statically linked winpthreads and
    libstdc++): the nearest preceding function symbol, from `nm`."""

    def __init__(self, exe: Path, base: int):
        out = subprocess.run([tool("nm"), "-C", "--defined-only", str(exe)],
                             capture_output=True, text=True, check=True).stdout
        syms = []
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1] in ("t", "T"):
                syms.append((int(parts[0], 16) - base, parts[2]))
        syms.sort()
        self.addrs = [a for a, _ in syms]
        self.names = [n for _, n in syms]

    def lookup(self, rva: int) -> str | None:
        i = bisect.bisect_right(self.addrs, rva) - 1
        return simplify(self.names[i]) if i >= 0 else None


def symbolise(exe: Path, rvas: list[int]) -> dict[int, list[str]]:
    """rva -> frames, outermost first (inline chain expanded)."""
    base = image_base(exe)
    symtab = SymbolTable(exe, base)
    result: dict[int, list[str]] = {}
    chunk = 2000
    for start in range(0, len(rvas), chunk):
        part = rvas[start:start + chunk]
        out = subprocess.run(
            [tool("addr2line"), "-a", "-f", "-C", "-i", "-e", str(exe)]
            + [hex(base + r) for r in part],
            capture_output=True, text=True, check=True).stdout.splitlines()
        current: int | None = None
        funcs: list[str] = []
        lines = iter(out)
        for line in lines:
            if line.startswith("0x"):
                if current is not None:
                    result[current] = funcs[::-1]
                current = int(line, 16) - base
                funcs = []
            else:
                next(lines, None)  # file:line
                if line != "??":
                    funcs.append(simplify(line))
                else:
                    funcs.append(symtab.lookup(current or 0) or f"exe+{hex(current or 0)}")
        if current is not None:
            result[current] = funcs[::-1]
    return result


def fold(exe: Path, raw: Path) -> Counter:
    stacks: list[tuple[list[str], int]] = []
    rvas: set[int] = set()
    for line in raw.read_text().splitlines():
        stack, _, count = line.rpartition(" ")
        frames = stack.split(";")
        stacks.append((frames, int(count)))
        rvas.update(int(f[4:], 16) for f in frames if f.startswith("exe+0x"))
    names = symbolise(exe, sorted(rvas))

    folded: Counter = Counter()
    for frames, count in stacks:
        thread, out = frames[0], []
        for f in frames[1:]:
            if f.startswith("exe+0x"):
                out.extend(names.get(int(f[4:], 16), [f]))
            else:
                out.append(f.replace(";", ":"))
        # Trim the thread-start plumbing (RtlUserThreadStart, CRT and
        # std::thread trampolines): root each stack at the workload function.
        start = next((i for i, f in enumerate(out) if ROOT_FRAME.match(f)), 0)
        folded[";".join([thread] + out[start:])] += count
    return folded


def flamegraph_pl() -> Path:
    script = FLAMEGRAPH_DIR / "flamegraph.pl"
    if not script.exists():
        print(f"fetching FlameGraph into {FLAMEGRAPH_DIR} ...")
        subprocess.run(["git", "clone", "--depth", "1", FLAMEGRAPH_REPO, str(FLAMEGRAPH_DIR)],
                       check=True)
    return script


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawTextHelpFormatter)
    ap.add_argument("exe", type=Path)
    ap.add_argument("raw", type=Path, help="raw stacks written by spsc_profile --out")
    ap.add_argument("svg", type=Path)
    ap.add_argument("--title", default="Flame graph")
    ap.add_argument("--subtitle", default="")
    args = ap.parse_args()

    folded = fold(args.exe, args.raw)
    folded_path = args.svg.with_suffix(".folded")
    # LF only, as bytes: flamegraph.pl rejects CRLF lines as "invalid format",
    # and Python's text mode on Windows would translate \n to \r\n.
    data = "".join(f"{s} {c}\n" for s, c in sorted(folded.items())).encode()
    folded_path.write_bytes(data)

    cmd = [tool("perl"), str(flamegraph_pl()), "--title", args.title, "--width", "1400",
           "--minwidth", "0.2", "--countname", "samples", "--colors", "hot"]
    if args.subtitle:
        cmd += ["--subtitle", args.subtitle]
    svg = subprocess.run(cmd, input=data, capture_output=True, check=True).stdout
    args.svg.write_bytes(svg)
    print(f"wrote {args.svg} ({sum(folded.values())} samples, folded stacks in {folded_path.name})")


if __name__ == "__main__":
    main()
