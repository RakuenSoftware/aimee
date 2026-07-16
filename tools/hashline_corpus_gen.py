#!/usr/bin/env python3
"""Generate a REALISTIC hashline eval corpus from real repository files.

The tiny hand-written corpus in benchmarks/hashline/fixtures.json cannot exercise
the regime where str_replace's failure modes bite (that is why the agentic eval
came back inconclusive/negative on it). This generator mutates real source files
to build verifiable before/after edit tasks in the categories the proposal's
benchmark stresses:

  collision   - the target line's exact text occurs many times in the file, so a
                bare str_replace old_string is ambiguous (must add context);
                hashline targets the ordinal directly.
  deep-indent - a deeply-indented line, testing whitespace recall for str_replace.
  whole-func  - rewrite an entire C function body (large old_string re-emit).
  drift       - a line is inserted above the target after the read, so ordinals
                shift; str_replace is content-addressed (robust), hashline must
                re-anchor. Included for honesty: this case can favor str_replace.

Every task's `expected` is constructed by explicit line surgery, so it is exact
and language-agnostic (we test edit MECHANICS, not compilation).

Usage:
    python3 tools/hashline_corpus_gen.py --src src --max-files 8 \
        --out benchmarks/hashline/corpus.generated.json
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any, Dict, List, Optional


def split_keep(text: str) -> List[str]:
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts = parts[:-1]
    return parts


def join(lines: List[str], trailing_nl: bool) -> str:
    return "\n".join(lines) + ("\n" if trailing_nl else "")


def substantive(line: str) -> bool:
    s = line.strip()
    return len(s) >= 12 and s not in ("{", "}", "*/", "/*", "return;", "break;", "continue;")


def mutate_line(line: str, tag: str) -> str:
    """A deterministic, plausible edit: append a distinctive trailing marker so the
    before/after differ unambiguously. Comment syntax varies by language, but the
    task only needs a well-defined string transform."""
    return line.rstrip() + f"  /* {tag} */"


def find_collision(lines: List[str]) -> Optional[int]:
    seen: Dict[str, List[int]] = {}
    for i, ln in enumerate(lines):
        if substantive(ln):
            seen.setdefault(ln, []).append(i + 1)
    # a line that repeats, target its LAST occurrence (deep in the file)
    for ln, ords in seen.items():
        if len(ords) >= 3:
            return ords[-1]
    for ln, ords in seen.items():
        if len(ords) >= 2:
            return ords[-1]
    return None


def find_deep_indent(lines: List[str]) -> Optional[int]:
    best = None
    for i, ln in enumerate(lines):
        indent = len(ln) - len(ln.lstrip(" \t"))
        if indent >= 8 and substantive(ln) and lines.count(ln) == 1:
            return i + 1
    return best


def find_function(lines: List[str]) -> Optional[tuple]:
    """Small Allman-style C-function finder: an opening '{' on its own line whose
    previous line is a signature (has '(' and ')'), brace-matched to its close.
    Returns (signature_line, close_line), both 1-based."""
    for i in range(1, len(lines)):
        prev = lines[i - 1].strip()
        if lines[i].strip() != "{":
            continue
        if "(" not in prev or ")" not in prev:
            continue
        if prev.startswith(("if", "for", "while", "switch", "else", "}", "*", "//")):
            continue
        depth = 0
        for j in range(i, min(i + 60, len(lines))):
            depth += lines[j].count("{") - lines[j].count("}")
            if depth == 0 and j > i:
                if 3 <= (j - (i - 1)) <= 40:
                    return (i, j + 1)  # signature line (i, 1-based) .. close (j+1)
                break
    return None


def find_unique(lines: List[str], min_line: int = 10) -> Optional[int]:
    """A unique substantive line deep enough to have real context above it (so an
    inserted line meaningfully shifts ordinals), skipping the header region."""
    for i, ln in enumerate(lines):
        if i + 1 <= min_line:
            continue
        s = ln.strip()
        if s.startswith("#") or s.startswith("//") or s.startswith("*"):
            continue
        if substantive(ln) and lines.count(ln) == 1:
            return i + 1
    return None


def make_tasks(path: Path, text: str, idx: int) -> List[Dict[str, Any]]:
    lines = split_keep(text)
    trailing = text.endswith("\n")
    if len(lines) < 12:
        return []
    tasks: List[Dict[str, Any]] = []
    name = path.name

    # collision
    c = find_collision(lines)
    if c:
        new = mutate_line(lines[c - 1], "e")
        exp = lines[:c - 1] + [new] + lines[c:]
        tasks.append({"name": f"{name}:collision", "category": "collision", "initial": text,
                      "target_line": c, "end_line": c, "new_text": new,
                      "expected": join(exp, trailing),
                      "note": f"target line occurs {lines.count(lines[c-1])}x"})

    # deep-indent
    d = find_deep_indent(lines)
    if d:
        new = mutate_line(lines[d - 1], "e")
        exp = lines[:d - 1] + [new] + lines[d:]
        tasks.append({"name": f"{name}:deep-indent", "category": "deep-indent", "initial": text,
                      "target_line": d, "end_line": d, "new_text": new,
                      "expected": join(exp, trailing)})

    # whole-function
    fn = find_function(lines)
    if fn:
        s, e = fn
        sig = lines[s - 1]
        body = [sig, "{", "   return 0; /* rewritten */", "}"]
        # only if the original span isn't already that shape
        exp = lines[:s - 1] + body + lines[e:]
        tasks.append({"name": f"{name}:whole-func", "category": "whole-func", "initial": text,
                      "target_line": s, "end_line": e, "new_text": "\n".join(body),
                      "expected": join(exp, trailing)})

    # drift: insert a line above a unique target after the read
    u = find_unique(lines)
    if u and u > 3:
        ins_at = u - 2  # 0-based index to insert before
        inserted = "   /* concurrent insert */"
        drifted = lines[:ins_at] + [inserted] + lines[ins_at:]
        new = mutate_line(lines[u - 1], "e")
        # target content moved down by 1 in the drifted file
        dt = u + 1
        exp = drifted[:dt - 1] + [new] + drifted[dt:]
        tasks.append({"name": f"{name}:drift", "category": "drift", "initial": text,
                      "target_line": u, "end_line": u, "new_text": new,
                      "drift_to": join(drifted, trailing), "expected": join(exp, trailing),
                      "note": "a line was inserted above the target after the read"})

    return tasks


def main() -> int:
    ap = argparse.ArgumentParser(description="generate a realistic hashline eval corpus")
    ap.add_argument("--src", type=Path, default=Path("src"))
    ap.add_argument("--max-files", type=int, default=8)
    ap.add_argument("--out", type=Path, default=Path("benchmarks/hashline/corpus.generated.json"))
    ap.add_argument("--glob", default="*.c")
    args = ap.parse_args()

    files = sorted(args.src.glob(args.glob))
    corpus: List[Dict[str, Any]] = []
    used = 0
    for f in files:
        if used >= args.max_files:
            break
        try:
            text = f.read_text()
        except Exception:
            continue
        # skip huge files to keep prompts affordable
        if not (400 <= len(text) <= 20000):
            continue
        tasks = make_tasks(f, text, used)
        if tasks:
            corpus.extend(tasks)
            used += 1

    # self-verify: every task's construction must be internally consistent
    for t in corpus:
        assert t["expected"] != t["initial"], f"no-op task {t['name']}"

    payload = {"description": "Realistic hashline eval corpus, mutation-generated from real files.",
               "generated_from": str(args.src), "count": len(corpus), "fixtures": corpus}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(payload, indent=1))
    by_cat: Dict[str, int] = {}
    for t in corpus:
        by_cat[t["category"]] = by_cat.get(t["category"], 0) + 1
    print(f"wrote {len(corpus)} tasks from {used} file(s) -> {args.out}")
    print("by category:", by_cat)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
