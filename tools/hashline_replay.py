#!/usr/bin/env python3
"""Model-roster replay for the hashline edit core (proposal Part I gate).

The deterministic half of the gate is `src/tests/test_hashline_gate.c` (runs in
CI, no models). THIS is the other half: replay the same fixture corpus across a
model roster and measure the proposal's headline metrics --

    * pass@1  : did the model land the edit first try (resulting file == expected)
    * output tokens per task, and the NET token delta (anchored-read overhead
      minus the edit-block bytes str_replace makes the model re-emit)

for both the legacy `str_replace` schema and the anchored `hashline` schema.

Ship criteria (from the proposal):
    hashline pass@1 >= str_replace on every model, strictly better on the
    local/open-weight delegates, and net token-negative overall.

Usage:
    # offline self-check: validate the corpus + scoring pipeline with a perfect
    # "oracle" model (no network). Exits 0 if the harness itself is sound.
    python3 tools/hashline_replay.py --mock

    # real run against an OpenAI-compatible chat endpoint (e.g. an aimee delegate
    # or llama.cpp server); repeat --model to build a roster.
    python3 tools/hashline_replay.py \
        --endpoint http://localhost:8080/v1/chat/completions \
        --model grok-code-fast-1 --model qwen2.5-coder-7b --runs 3

Exit codes:
    0  harness ran; gate criteria met (or --mock self-check passed)
    1  gate criteria NOT met
    2  usage / environment error
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURES = ROOT / "benchmarks" / "hashline" / "fixtures.json"

# ~4 chars/token is the standard rough proxy; we report bytes AND this estimate.
CHARS_PER_TOKEN = 4.0


# --------------------------------------------------------------------------- #
# corpus                                                                       #
# --------------------------------------------------------------------------- #
def load_fixtures(path: Path) -> List[Dict[str, Any]]:
    data = json.loads(path.read_text())
    fx = data.get("fixtures", [])
    if not fx:
        raise SystemExit(f"no fixtures in {path}")
    return fx


def lines(text: str) -> List[str]:
    # keep the trailing-newline shape: "a\nb\n" -> ["a","b"], "a\nb" -> ["a","b"]
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts = parts[:-1]
    return parts


# --------------------------------------------------------------------------- #
# reference edit application (how we SCORE a model's proposed edit)            #
# --------------------------------------------------------------------------- #
def apply_str_replace(initial: str, old: str, new: str) -> Tuple[Optional[str], str]:
    """Return (result, note). None result == the edit was rejected/failed."""
    count = initial.count(old)
    if count == 0:
        return None, "old_string not found"
    if count > 1:
        return None, f"old_string occurs {count}x (ambiguous)"
    return initial.replace(old, new, 1), "applied"


def apply_hashline(current: str, snapshot: str, ordinal: int, end: int, new: str) -> Tuple[Optional[str], str]:
    """Mirror the server's anchored replace_range against `current`, verifying the
    cited ordinals still match the snapshot the anchors came from."""
    snap_lines = lines(snapshot)
    cur_lines = lines(current)
    if ordinal < 1 or end > len(snap_lines):
        return None, "anchor out of range"
    # freshness: the current bytes at each cited ordinal must equal the snapshot's
    for k in range(ordinal, end + 1):
        if k > len(cur_lines) or cur_lines[k - 1] != snap_lines[k - 1]:
            return None, "stale_anchor"
    out = cur_lines[: ordinal - 1] + new.split("\n") + cur_lines[end:]
    trailing = "\n" if current.endswith("\n") else ""
    return "\n".join(out) + trailing, "applied"


# --------------------------------------------------------------------------- #
# model drivers                                                               #
# --------------------------------------------------------------------------- #
def oracle_edit(fx: Dict[str, Any], protocol: str) -> Dict[str, Any]:
    """A perfect model: emits the ideal edit for the protocol. Used by --mock to
    prove the scoring pipeline end-to-end without a network."""
    init = fx["initial"]
    src_lines = lines(init)
    o, e = fx["target_line"], fx.get("end_line", fx["target_line"])
    if protocol == "str_replace":
        old = "\n".join(src_lines[o - 1 : e])
        return {"old_string": old, "new_string": fx["new_text"]}
    return {"ordinal": o, "end": e, "new_text": fx["new_text"]}


def model_edit(fx: Dict[str, Any], protocol: str, endpoint: str, model: str) -> Dict[str, Any]:
    """Ask a real OpenAI-compatible chat model to produce the edit. Returns the
    parsed tool arguments plus the response's output-token count."""
    init = fx["initial"]
    if protocol == "str_replace":
        sys_p = (
            "You edit files with a str_replace tool: reply with ONLY a JSON object "
            '{"old_string": "...", "new_string": "..."}. old_string must match the '
            "file exactly and be unique."
        )
        user_p = f"File:\n{init}\nRewrite line {fx['target_line']} to: {fx['new_text']!r}"
    else:
        # hand the model the anchored view it would get from read_file
        anchored = "\n".join(f"{i+1}:xx| {ln}" for i, ln in enumerate(lines(init)))
        sys_p = (
            "You edit files by anchor: reply with ONLY a JSON object "
            '{"ordinal": N, "end": M, "new_text": "..."} citing the LINE numbers '
            "shown. You never reproduce the old lines."
        )
        user_p = f"snapshot=sMOCK\n{anchored}\nRewrite line {fx['target_line']} to: {fx['new_text']!r}"

    body = json.dumps(
        {
            "model": model,
            "messages": [
                {"role": "system", "content": sys_p},
                {"role": "user", "content": user_p},
            ],
            "temperature": 0,
        }
    ).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    key = os.environ.get("AIMEE_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    with urllib.request.urlopen(req, timeout=60) as r:
        resp = json.loads(r.read())
    content = resp["choices"][0]["message"]["content"]
    out_tokens = resp.get("usage", {}).get("completion_tokens")
    try:
        args = json.loads(content[content.index("{") : content.rindex("}") + 1])
    except Exception:
        args = {}
    if out_tokens is None:
        out_tokens = len(content) / CHARS_PER_TOKEN
    args["_out_tokens"] = out_tokens
    return args


# --------------------------------------------------------------------------- #
# scoring                                                                     #
# --------------------------------------------------------------------------- #
def score_fixture(fx: Dict[str, Any], protocol: str, edit: Dict[str, Any]) -> Dict[str, Any]:
    init = fx["initial"]
    current = fx.get("drift_to", init)  # the file the edit actually lands on
    expected = fx["expected"]
    drift = "drift_to" in fx

    if protocol == "str_replace":
        result, note = apply_str_replace(current, edit.get("old_string", ""), edit.get("new_string", ""))
        # bytes the model had to re-emit that it already read
        reproduced = len(edit.get("old_string", ""))
        out_bytes = reproduced + len(edit.get("new_string", ""))
        in_bytes = len(init)
    else:
        result, note = apply_hashline(
            current, init, int(edit.get("ordinal", 0)), int(edit.get("end", edit.get("ordinal", 0))), edit.get("new_text", "")
        )
        reproduced = 0
        out_bytes = len("sMOCK") + 8 + len(edit.get("new_text", ""))  # snapshot + anchors + new
        in_bytes = len(init) + 6 * len(lines(init))  # anchored-read overhead ~6 B/line

    # pass@1: file lands exactly on expected. For a drift fixture the SAFE outcome
    # is a clean reject (result is None) leaving the drifted file intact.
    if drift:
        passed = result is None
        safe = result is None or result == current
    else:
        passed = result == expected
        safe = passed or result is None

    return {
        "passed": passed,
        "safe": safe,
        "note": note,
        "reproduced": reproduced,
        "out_bytes": out_bytes,
        "in_bytes": in_bytes,
        "out_tokens": edit.get("_out_tokens", out_bytes / CHARS_PER_TOKEN),
    }


def run(models: List[str], endpoint: Optional[str], runs: int, fixtures: List[Dict[str, Any]], mock: bool) -> int:
    roster = models if models else (["oracle"] if mock else [])
    if not roster:
        print("no models: pass --mock for an offline self-check, or --model NAME --endpoint URL", file=sys.stderr)
        return 2

    overall_ok = True
    print(f"hashline model-roster replay  ({len(fixtures)} fixtures x {len(roster)} model(s) x {runs} run(s))\n")
    for model in roster:
        agg = {p: {"pass": 0, "safe": 0, "n": 0, "out": 0, "in": 0} for p in ("str_replace", "hashline")}
        for _ in range(runs):
            for fx in fixtures:
                for proto in ("str_replace", "hashline"):
                    if mock or model == "oracle":
                        edit = oracle_edit(fx, proto)
                    else:
                        assert endpoint
                        edit = model_edit(fx, proto, endpoint, model)
                    s = score_fixture(fx, proto, edit)
                    a = agg[proto]
                    a["pass"] += int(s["passed"])
                    a["safe"] += int(s["safe"])
                    a["n"] += 1
                    a["out"] += s["out_bytes"]
                    a["in"] += s["in_bytes"]

        sr, hl = agg["str_replace"], agg["hashline"]
        sr_p1 = sr["pass"] / sr["n"]
        hl_p1 = hl["pass"] / hl["n"]
        net = (hl["in"] + hl["out"]) - (sr["in"] + sr["out"])
        net_pct = 100.0 * net / (sr["in"] + sr["out"]) if (sr["in"] + sr["out"]) else 0.0
        print(f"  {model:<24} pass@1  str_replace {sr_p1:5.1%}   hashline {hl_p1:5.1%}   "
              f"safe {hl['safe']}/{hl['n']}   net tokens {net_pct:+.0f}%")
        # per-model gate: hashline pass@1 must not regress
        if hl_p1 + 1e-9 < sr_p1:
            overall_ok = False
            print(f"    ! REGRESSION: hashline pass@1 below str_replace for {model}")

    print("\nGate: hashline pass@1 >= str_replace on every model"
          + ("  -> PASS" if overall_ok else "  -> FAIL"))
    if mock:
        print("(--mock: oracle model, exercises the corpus + scoring pipeline only; "
              "real pass@1 needs --endpoint)")
    return 0 if overall_ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="hashline model-roster replay (proposal Part I gate)")
    ap.add_argument("--fixtures", type=Path, default=DEFAULT_FIXTURES)
    ap.add_argument("--endpoint", help="OpenAI-compatible /v1/chat/completions URL")
    ap.add_argument("--model", action="append", default=[], help="model id (repeatable)")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--mock", action="store_true", help="offline self-check with a perfect oracle model")
    args = ap.parse_args()

    if not args.mock and not args.endpoint:
        print("error: pass --mock (offline) or --endpoint URL --model NAME (live)", file=sys.stderr)
        return 2
    fixtures = load_fixtures(args.fixtures)
    return run(args.model, args.endpoint, args.runs, fixtures, args.mock)


if __name__ == "__main__":
    sys.exit(main())
