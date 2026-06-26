#!/usr/bin/env python3
"""A/B token benchmark for aimee's Codex (/v1/responses) ingress pre-injection.

Measures how much server-side context pre-injection (config:
``ingress_preinject_enabled``) reduces the tokens Codex spends on a turn. For
each prompt the harness runs ``codex exec --json`` against aimee's ``/v1``
endpoint twice — once with the A/B flag ON, once OFF — and reads the real token
usage from the ``turn.completed`` event
(``input_tokens + cached_input_tokens + output_tokens``).

Two modes (``--mode``):

* ``preinject`` (default) — A/B ``ingress_preinject_enabled``: the original
  envelope-on/off token benchmark.
* ``compress`` — the **§6 lossy-fold net-token gate**. Pre-injection is pinned
  ON and ``ingress_compress_enabled`` is A/B'd, so the comparison isolates the
  code-fold (file:line references, recovered via ``code_span_get``). Because the
  per-turn total already includes any ``code_span_get`` recovery the model made,
  ``saved = off - on`` is the **net** economics (resident savings minus recovery
  round-trips), exactly the §6 metric — *not* gross resident reduction. Results
  break down per §6.5 B6 task class (tag prompts ``[class] prompt``); a class
  whose net is negative must keep folds non-lossy / not flip the default. The
  forced-rehydration **accuracy** A/B (does the model recover when the
  folded-out detail is required?) needs a labelled gold set and a live model and
  runs separately via ``benchmarks/learning/learning_replay.py``.

Toggle mechanism
----------------
P1 gates pre-injection on the server config flag, which the server re-reads per
request (``config_load``), so flipping it takes effect with no redeploy. The
harness flips it between the two runs of each prompt via the aimee CLI:

    aimee config set ingress_preinject_enabled 1   # ON
    aimee config set ingress_preinject_enabled 0   # OFF

and restores the original value at the end. The server also supports the
``x-aimee-preinject: 0`` request header for per-request disable, but ``codex
exec`` does not expose arbitrary provider headers, so the benchmark uses the
config toggle.

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
                                         [--mode preinject|compress]

    # the §6 lossy-fold net-token gate, per task class:
    python3 bench/ingress_token_bench.py --mode compress \\
        --prompts bench/ingress_compress_prompts.txt

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
    env = dict(os.environ)
    # The aimee codex model provider authenticates with the loopback bearer; the
    # provider block sets env_key = "AIMEE_API_KEY". Fall back to the remote
    # bearer if AIMEE_API_KEY is unset.
    env.setdefault("AIMEE_API_KEY", os.environ.get("AIMEE_BEARER", "aimee-local-dev"))
    return subprocess.run(cmd, cwd=str(cwd) if cwd else None, text=True,
                          capture_output=True, env=env)


def set_flag(flag: str, value: int) -> bool:
    """Set a server config flag; returns True on apparent success."""
    res = run(["aimee", "config", "set", flag, str(value)])
    if res.returncode != 0:
        log(f"aimee config set {flag} {value} failed: {res.stderr.strip()[:200]}")
        return False
    return True


def get_flag(flag: str) -> int | None:
    res = run(["aimee", "config", "get", flag])
    if res.returncode != 0:
        return None
    out = (res.stdout or "").strip().lower()
    # Tolerate "true"/"1"/"<flag>: 1" style output.
    if "1" in out or "true" in out or "on" in out:
        return 1
    if "0" in out or "false" in out or "off" in out:
        return 0
    return None


# Wrappers preserving the original preinject-mode call sites.
def aimee_set_flag(value: int) -> bool:
    return set_flag(FLAG, value)


def aimee_get_flag() -> int | None:
    return get_flag(FLAG)


# §6.5 B6 task classes; the per-class breakdown the §6 gates require. A prompt is
# labelled by a leading "[class]" tag in the prompts file (else "unlabelled").
TASK_CLASSES = ("code_generation", "code_review", "debugging", "question_answer",
                "summarization", "agent_tool_loop")


def split_task_class(line: str) -> tuple[str, str]:
    """Split a leading "[task_class] prompt" tag; default 'unlabelled'."""
    if line.startswith("[") and "]" in line:
        tag, rest = line[1:].split("]", 1)
        tag = tag.strip()
        if tag in TASK_CLASSES:
            return tag, rest.strip()
    return "unlabelled", line


def codex_tokens(prompt: str, project: Path, model: str) -> tuple[int, str] | None:
    """Run one prompt through codex; return (total_tokens, final_text) or None."""
    provider = os.environ.get("AIMEE_BENCH_PROVIDER", "aimee")
    cmd = ["codex", "exec", "--json", "--skip-git-repo-check", "-C", str(project),
           "-c", f"model_provider={provider}"]
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
    ap.add_argument("--mode", choices=("preinject", "compress"), default="preinject",
                    help="preinject: A/B ingress_preinject_enabled (default). "
                         "compress: pin pre-injection ON and A/B ingress_compress_enabled — the "
                         "§6 lossy-fold net-token gate. The 'on' total already includes any "
                         "code_span_get recovery the model made that turn, so saved = off-on is the "
                         "NET economics (resident savings minus recovery round-trips), not gross.")
    args = ap.parse_args()

    # The flag under A/B, and any flag pinned ON for the run (compress only matters
    # when pre-injection is on, so it is pinned for the compress arm).
    ab_flag = FLAG if args.mode == "preinject" else "ingress_compress_enabled"
    pin_flag = None if args.mode == "preinject" else FLAG

    prompts_path = Path(args.prompts)
    if not prompts_path.exists():
        log(f"prompts file not found: {prompts_path}")
        return 2
    prompts = [split_task_class(ln.strip()) for ln in prompts_path.read_text().splitlines()
               if ln.strip() and not ln.startswith("#")]
    if not prompts:
        log("no prompts to run")
        return 2
    project = Path(args.project)

    original = get_flag(ab_flag)
    pin_original = get_flag(pin_flag) if pin_flag else None
    log(f"mode={args.mode} A/B flag={ab_flag} (original={original})"
        + (f" pin {pin_flag}=1 (original={pin_original})" if pin_flag else ""))

    rows = []
    try:
        if pin_flag and not set_flag(pin_flag, 1):
            log(f"could not pin {pin_flag}=1; aborting")
            return 1
        for i, (task_class, prompt) in enumerate(prompts, 1):
            log(f"prompt {i}/{len(prompts)} [{task_class}]: {prompt[:54]!r}")
            if not set_flag(ab_flag, 1):
                continue
            on = codex_tokens(prompt, project, args.model)
            if not set_flag(ab_flag, 0):
                continue
            off = codex_tokens(prompt, project, args.model)
            if not on or not off:
                log("  skipped (a run failed)")
                continue
            on_tok, off_tok = on[0], off[0]
            saved = off_tok - on_tok
            pct = (saved / off_tok * 100.0) if off_tok else 0.0
            row = {"prompt": prompt, "task_class": task_class, "on_tokens": on_tok,
                   "off_tokens": off_tok, "saved": saved, "reduction_pct": round(pct, 1)}
            rows.append(row)
            if args.jsonl:
                print(json.dumps(row), flush=True)
            log(f"  on={on_tok} off={off_tok} net_saved={saved} ({pct:.1f}%)")
    finally:
        if original is not None:
            set_flag(ab_flag, original)
            log(f"restored {ab_flag} = {original}")
        if pin_flag and pin_original is not None:
            set_flag(pin_flag, pin_original)
            log(f"restored {pin_flag} = {pin_original}")

    if not rows:
        log("no successful prompt pairs")
        return 1

    label = "net (incl. recovery)" if args.mode == "compress" else "saved"
    print(f"\n=== ingress {args.mode} token A/B ({ab_flag}) ===")
    print(f"{'prompt':36.36}  {'class':16.16}  {'off':>8}  {'on':>8}  {label:>14}  {'%':>6}")
    for r in rows:
        print(f"{r['prompt']:36.36}  {r['task_class']:16.16}  {r['off_tokens']:>8}  "
              f"{r['on_tokens']:>8}  {r['saved']:>14}  {r['reduction_pct']:>5.1f}")
    tot_off = sum(r["off_tokens"] for r in rows)
    tot_on = sum(r["on_tokens"] for r in rows)
    overall = (tot_off - tot_on) / tot_off * 100.0 if tot_off else 0.0
    mean_pct = statistics.mean(r["reduction_pct"] for r in rows)
    print(f"\npairs: {len(rows)}   total off={tot_off}  on={tot_on}")
    print(f"overall {label}: {overall:.1f}%   mean per-prompt: {mean_pct:.1f}%")

    # §6 B6: per-task-class breakdown — the net-token gate is evaluated PER CLASS
    # (a class where recovery erases the saving must not flip the default).
    by_class: dict[str, list[dict]] = {}
    for r in rows:
        by_class.setdefault(r["task_class"], []).append(r)
    if len(by_class) > 1 or args.mode == "compress":
        print("\nby task class:")
        print(f"  {'class':18.18}  {'pairs':>5}  {'off':>8}  {'on':>8}  {'net%':>6}")
        for cls in sorted(by_class):
            crs = by_class[cls]
            coff = sum(r["off_tokens"] for r in crs)
            con = sum(r["on_tokens"] for r in crs)
            cpct = (coff - con) / coff * 100.0 if coff else 0.0
            flag = "  <-- net NEGATIVE (do not flip)" if cpct < 0 else ""
            print(f"  {cls:18.18}  {len(crs):>5}  {coff:>8}  {con:>8}  {cpct:>5.1f}{flag}")
    if args.mode == "compress":
        log("NOTE: forced-rehydration ACCURACY A/B (does the model recover when the folded-out "
            "detail is required?) needs a labelled gold set + a live model and is run separately "
            "via learning_replay.py; this harness measures the net-token gate only.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
