#!/usr/bin/env python3
"""Emit differential-test samples for a served spec.

EVERY POSITIONAL COUNT, not just the full one. index.structure shipped a wrong
spec because the samples only ever supplied two positionals and three: the
marshaller sends <project> <file_path> for two and <file_path> alone for ONE,
and the one-positional case -- the way the command is actually used -- was never
generated. `aimee index structure <file>` sent the file as a project and the
server answered "missing file_path". The differential test agreed with itself
the whole time.

A sample set built from a spec can only probe what the spec mentions. It can at
least probe every ARITY the spec implies, which is cheap and would have caught
this one.
"""
import re


def samples_for(spec):
    out = [[]]
    if not spec:
        return out
    flags, pos = [], []
    for f in spec.get("fields", []):
        src = f.get("from")
        if src in ("flag", "positional_or_flag", "flag_or_positional") and f.get("flag"):
            flags.append(f["flag"])
        if src in ("positional", "positional_or_flag", "flag_or_positional"):
            pos.append(f.get("index", 0))
        for s in f.get("sources", []) or []:
            if s.get("from") == "flag" and s.get("flag"):
                flags.append(s["flag"])
            if s.get("from") == "positional":
                pos.append(s.get("index", 0))

    seen = set()
    flags = [f for f in flags if not (f in seen or seen.add(f))]
    for fl in flags:
        out.append(["--" + fl, "v"])
    if flags:
        out.append([x for fl in flags[:3] for x in ("--" + fl, "v")])
        out.append(["--" + flags[0], ""])
        out.append(["--" + flags[0], "12x"])

    if pos:
        n = max(pos) + 1
        # 1..n+1, and the empty form at each width.
        for k in range(1, n + 2):
            out.append([f"p{i}" for i in range(k)])
        for k in range(1, n + 1):
            out.append([""] * k)

    out.append(["--unknown-flag", "x"])
    return [s for s in out if len(s) < 8]
