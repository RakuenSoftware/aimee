#!/usr/bin/env python3
"""A/B token benchmark for aimee's Codex (/v1/responses) ingress pre-injection.

Measures how much server-side context pre-injection (config:
``ingress_preinject_enabled``) reduces the tokens Codex spends on a turn. For
each prompt the harness runs ``codex exec --json`` against aimee's ``/v1``
endpoint twice — once with pre-injection ON, once OFF — and reads the real token
usage from the ``turn.completed`` event
(``input_tokens + cached_input_tokens + output_tokens``).

Toggle mechanism
----------------
P1 gates pre-injection on the server config flag, which the server re-reads per
request (``config_load``), so flipping it takes effect with no redeploy. The
harness flips it between the two runs of each prompt via the aimee CLI:

    aimee config set ingress_preinject_enabled 1   # ON
    aimee config set ingress_preinject_enabled 0   # OFF

and restores the original value at the end. (A future ``x-aimee-preinject``
request header would avoid mutating shared server state; until then this is the
runtime toggle.)

Prerequisites
-------------
* ``codex`` on PATH, configured with an aimee model provider pointed at
  ``$AIMEE_SERVER_URL/v1`` (wire_api = "responses"); see
  docs/proposals/pending/codex-frontend-ingress.md.
* ``aimee`` on PATH with a remote/server set (``aimee remote``), so
  ``aimee config set`` reaches the same server Codex talks to.
* A prompts file (default ``bench/ingress_prompts.txt``), one prompt per line.

Usage
-----
    python3 bench/ingress_token_bench.py [--prompts FILE] [--project DIR]
                                         [--jsonl] [--model NAME]

This harness does NOT modify repository files; prompts should ask Codex to
answer briefly without editing code.
"""
from __future__ import annotations

import argparse
import json
import os
import statistics
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
DEFAULT_PROMPTS = REPO_ROOT / "bench" / "ingress_prompts.txt"
FLAG = "ingress_preinject_enabled"


def log(msg: str) -> None:
    print(f"[bench] {msg}", file=sys.stderr, flush=True)


def run(cmd: list[str], cwd: Path | None = None) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, text=True, capture_output=True)


def aimee_set_flag(value: int) -> bool:
    """Set the server pre-injection flag; returns True on apparent success."""
    res = run(["aimee", "config", "set", FLAG, str(value)])
    if res.returncode != 0:
        log(f"aimee config set {FLAG} {value} failed: {res.stderr.strip()[:200]}")
        return False
    return True


def aimee_get_flag() -> int | None:
    res = run(["aimee", "config", "get", FLAG])
    if res.returncode != 0:
        return None
    out = (res.stdout or "").strip().lower()
    # Tolerate "true"/"1"/"ingress_preinject_enabled: 1" style output.
    if "1" in out or "true" in out or "on" in out:
        return 1
    if "0" in out or "false" in out or "off" in out:
        return 0
    return None


def codex_tokens(prompt: str, project: Path, model: str) -> tuple[int, str] | None:
    """Run one prompt through codex; return (total_tokens, final_text) or None."""
    cmd = ["codex", "exec", "--json", "--skip-git-repo-check", "-C", str(project)]
    if model:
        cmd += ["-m", model]
    cmd.append(prompt)
    res = run(cmd)
    if res.returncode != 0:
        log(f"codex exec failed (rc={res.returncode}): {res.stderr.strip()[-300:]}")
        return None
    total, last_text = 0, ""
    for line in res.stdout.splitlines():
        try:
            obj = json.loads(line)
        except Exception:
            continue
        if obj.get("type") == "item.completed":
            item = obj.get("item", {})
            if item.get("type") == "agent_message":
                last_text = str(item.get("text", "")).strip()
        if obj.get("type") == "turn.completed":
            u = obj.get("usage", {})
            total = (int(u.get("input_tokens", 0))
                     + int(u.get("cached_input_tokens", 0))
                     + int(u.get("output_tokens", 0)))
    if total <= 0:
        log(f"no token usage in codex output: {res.stdout[-300:]}")
        return None
    return total, last_text


def main() -> int:
    ap = argparse.ArgumentParser(description="A/B token benchmark for the aimee Codex ingress.")
    ap.add_argument("--prompts", default=str(DEFAULT_PROMPTS))
    ap.add_argument("--project", default=str(REPO_ROOT),
                    help="working dir codex runs in (read-only prompts)")
    ap.add_argument("--model", default=os.environ.get("AIMEE_BENCH_MODEL", "aimee"))
    ap.add_argument("--jsonl", action="store_true", help="emit raw per-prompt results as JSONL")
    args = ap.parse_args()

    prompts_path = Path(args.prompts)
    if not prompts_path.exists():
        log(f"prompts file not found: {prompts_path}")
        return 2
    prompts = [ln.strip() for ln in prompts_path.read_text().splitlines()
               if ln.strip() and not ln.startswith("#")]
    if not prompts:
        log("no prompts to run")
        return 2
    project = Path(args.project)

    original = aimee_get_flag()
    log(f"original {FLAG} = {original}")

    rows = []
    try:
        for i, prompt in enumerate(prompts, 1):
            log(f"prompt {i}/{len(prompts)}: {prompt[:60]!r}")
            if not aimee_set_flag(1):
                continue
            on = codex_tokens(prompt, project, args.model)
            if not aimee_set_flag(0):
                continue
            off = codex_tokens(prompt, project, args.model)
            if not on or not off:
                log("  skipped (a run failed)")
                continue
            on_tok, off_tok = on[0], off[0]
            saved = off_tok - on_tok
            pct = (saved / off_tok * 100.0) if off_tok else 0.0
            row = {"prompt": prompt, "on_tokens": on_tok, "off_tokens": off_tok,
                   "saved": saved, "reduction_pct": round(pct, 1)}
            rows.append(row)
            if args.jsonl:
                print(json.dumps(row), flush=True)
            log(f"  on={on_tok} off={off_tok} saved={saved} ({pct:.1f}%)")
    finally:
        if original is not None:
            aimee_set_flag(original)
            log(f"restored {FLAG} = {original}")

    if not rows:
        log("no successful prompt pairs")
        return 1

    print("\n=== ingress pre-injection token A/B ===")
    print(f"{'prompt':40.40}  {'off':>9}  {'on':>9}  {'saved':>8}  {'%':>6}")
    for r in rows:
        print(f"{r['prompt']:40.40}  {r['off_tokens']:>9}  {r['on_tokens']:>9}  "
              f"{r['saved']:>8}  {r['reduction_pct']:>5.1f}")
    tot_off = sum(r["off_tokens"] for r in rows)
    tot_on = sum(r["on_tokens"] for r in rows)
    overall = (tot_off - tot_on) / tot_off * 100.0 if tot_off else 0.0
    mean_pct = statistics.mean(r["reduction_pct"] for r in rows)
    print(f"\npairs: {len(rows)}   total off={tot_off}  on={tot_on}")
    print(f"overall reduction: {overall:.1f}%   mean per-prompt: {mean_pct:.1f}%")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
