#!/usr/bin/env python3
"""Multi-turn AGENTIC eval for the hashline edit core (proposal Part I).

Why this exists: the one-shot `tools/hashline_replay.py` is not a fair test of
the proposal's claim. It scores a single JSON emission, which (a) penalises the
unfamiliar anchored schema vs the ubiquitous str_replace format and (b) never
credits the actual mechanism -- fewer RETRY LOOPS. This harness fixes both:

  * it renders the REAL anchored read (real FNV-1a-64 line hashes, same as
    src/anchor_snapshot.c) so the model sees exactly what aimee emits;
  * it runs a MULTI-TURN loop: the model proposes an edit, the harness applies
    it with the REAL tool semantics, and on failure feeds the REAL structured
    error back (str_replace "occurs N times" / "not found"; hashline
    "stale_anchor" with re-anchored context) so the model retries -- exactly the
    loop the anchor design is meant to shorten.

Metrics per (model, protocol): pass@1 (first-try), pass@k (within max_turns),
mean turns-to-success, mean completion tokens. The proposal's win is: equal-or-
better pass@k at FEWER turns and FEWER tokens.

Usage:
    python3 tools/hashline_agentic_eval.py --mock            # offline plumbing check
    python3 tools/hashline_agentic_eval.py \
        --endpoint https://HOST:8743/v1/chat/completions --insecure --token TOK \
        --model mimo-v2.5-pro --model MiniMax-M3 --runs 2 --max-turns 4
"""

from __future__ import annotations

import argparse
import json
import os
import ssl
import sys
import urllib.request
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_FIXTURES = ROOT / "benchmarks" / "hashline" / "fixtures.json"
_TLS_CTX: Optional[ssl.SSLContext] = None
_BEARER: Optional[str] = None


# --------------------------------------------------------------------------- #
# line model + hashing (must match src/anchor_snapshot.c)                      #
# --------------------------------------------------------------------------- #
FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def _canon(line: str, is_first: bool) -> bytes:
    b = line.encode("utf-8")
    if is_first and b[:3] == b"\xef\xbb\xbf":
        b = b[3:]
    return b  # newline already excluded by our split


def line_digest(line: str, is_first: bool) -> int:
    h = FNV64_OFFSET
    for c in _canon(line, is_first):
        h = ((h ^ c) * FNV64_PRIME) & MASK64
    return h


def short_tag(d: int) -> str:
    return f"{d & 0xff:02x}"


def split_lines(text: str) -> List[str]:
    parts = text.split("\n")
    if parts and parts[-1] == "":
        parts = parts[:-1]
    return parts


def render_anchored(text: str, snapshot_id: str) -> Tuple[str, List[int]]:
    ls = split_lines(text)
    digests = [line_digest(ln, i == 0) for i, ln in enumerate(ls)]
    out = [f"# anchored read snapshot={snapshot_id} (edit by anchor; 'LINE:HASH' cites a line)"]
    for i, ln in enumerate(ls):
        out.append(f"{i+1}:{short_tag(digests[i])}| {ln}")
    return "\n".join(out), digests


# --------------------------------------------------------------------------- #
# real tool semantics (mirror of tool_edit_file / edit_anchored.c)            #
# --------------------------------------------------------------------------- #
def strip_anchor_prefix(line: str) -> str:
    import re

    return re.sub(r"^\d+:[0-9a-fA-F]+\| ?", "", line)


def apply_str_replace(current: str, old: str, new: str) -> Tuple[Optional[str], str]:
    n = current.count(old)
    if not old:
        return None, "error: missing or empty 'old_string'"
    if n == 0:
        return None, "error: old_string not found in file; copy the exact text (incl. whitespace)"
    if n > 1:
        return None, f"error: old_string occurs {n} times; add surrounding context to make it unique"
    return current.replace(old, new, 1), "ok"


def apply_hashline(current: str, snap_digests: List[int], snapshot_id: str,
                   edits: List[Dict[str, Any]]) -> Tuple[Optional[str], str]:
    cur = split_lines(current)
    cur_digests = [line_digest(ln, i == 0) for i, ln in enumerate(cur)]

    def parse_anchor(tok: str) -> Optional[int]:
        try:
            return int(str(tok).split(":", 1)[0])
        except Exception:
            return None

    # freshness against the snapshot the anchors came from
    stale = []
    for e in edits:
        op = e.get("op")
        if op in ("replace", "insert_after"):
            lo = hi = parse_anchor(e.get("at"))
        else:
            lo, hi = parse_anchor(e.get("from")), parse_anchor(e.get("to"))
        if lo is None or hi is None:
            return None, 'error: {"status":"invalid_edit","reason":"malformed anchor"}'
        for ordn in range(lo, hi + 1):
            if ordn < 1 or ordn > len(snap_digests):
                stale.append(ordn)
            elif ordn > len(cur) or cur_digests[ordn - 1] != snap_digests[ordn - 1]:
                stale.append(ordn)
    if stale:
        # re-anchored context around the contested range (like the real server)
        lo = max(1, min(stale) - 3)
        hi = min(len(cur), max(stale) + 3)
        ctx = [{"anchor": f"{i}:{short_tag(cur_digests[i-1])}", "text": cur[i - 1]} for i in range(lo, hi + 1)]
        fresh_id = snapshot_id + "b"
        return None, json.dumps({"status": "stale_anchor", "snapshot_id": fresh_id, "context": ctx,
                                 "hint": "file changed since read; re-anchor from context"})

    # apply bottom-first
    out = list(cur)
    ops = []
    for e in edits:
        op = e.get("op")
        if op == "replace":
            k = parse_anchor(e.get("at"))
            ops.append((k, k, [strip_anchor_prefix(l) for l in str(e.get("text", "")).split("\n")]))
        elif op == "replace_range":
            ops.append((parse_anchor(e.get("from")), parse_anchor(e.get("to")),
                        [strip_anchor_prefix(l) for l in str(e.get("text", "")).split("\n")]))
        elif op == "delete_range":
            ops.append((parse_anchor(e.get("from")), parse_anchor(e.get("to")), []))
        elif op == "insert_after":
            k = parse_anchor(e.get("at"))
            ops.append((k + 1, k, [strip_anchor_prefix(l) for l in str(e.get("text", "")).split("\n")]))
        else:
            return None, 'error: {"status":"invalid_edit","reason":"unknown op"}'
    for lo, hi, repl in sorted(ops, key=lambda o: o[0], reverse=True):
        if hi >= lo:  # replace / replace_range / delete_range (repl == [] deletes)
            out[lo - 1: hi] = repl
        else:  # insert_after: lo == anchor + 1
            out[lo - 1: lo - 1] = repl
    trailing = "\n" if current.endswith("\n") else ""
    return "\n".join(out) + (trailing if out else ""), "ok"


# --------------------------------------------------------------------------- #
# model driver                                                                #
# --------------------------------------------------------------------------- #
def chat(messages: List[Dict[str, str]], endpoint: str, model: str) -> Tuple[str, float]:
    body = json.dumps({"model": model, "messages": messages, "temperature": 0}).encode()
    req = urllib.request.Request(endpoint, data=body, headers={"Content-Type": "application/json"})
    key = _BEARER or os.environ.get("AIMEE_API_KEY") or os.environ.get("OPENAI_API_KEY")
    if key:
        req.add_header("Authorization", f"Bearer {key}")
    with urllib.request.urlopen(req, timeout=120, context=_TLS_CTX) as r:
        resp = json.loads(r.read())
    content = resp["choices"][0]["message"]["content"]
    toks = resp.get("usage", {}).get("completion_tokens") or (len(content) / 4.0)
    return content, float(toks)


def parse_json(content: str) -> Dict[str, Any]:
    try:
        return json.loads(content[content.index("{"): content.rindex("}") + 1])
    except Exception:
        return {}


SR_SYS = ('You edit a file with a str_replace tool. Reply with ONLY a JSON object '
          '{"old_string": "...", "new_string": "..."}. old_string must match the file '
          'exactly (whitespace included) and be UNIQUE. If a tool error comes back, fix '
          'your call and try again.')
HL_SYS = ('You edit a file by anchor. The file is shown with "LINE:HASH| " prefixes. Reply '
          'with ONLY a JSON object {"snapshot_id": "...", "edits": [ ... ]} where each edit is '
          '{"op":"replace","at":"LINE:HASH","text":"..."} or replace_range(from,to,text) / '
          'insert_after(at,text) / delete_range(from,to). "text" is the raw line WITHOUT the '
          '"LINE:HASH| " prefix. If a stale_anchor error comes back, re-anchor from its context '
          'and retry.')


def run_task(fx: Dict[str, Any], proto: str, model: str, endpoint: Optional[str],
             max_turns: int, mock: bool) -> Dict[str, Any]:
    initial = fx["initial"]
    current = fx.get("drift_to", initial)  # the file the edit actually lands on
    expected = fx["expected"]
    is_drift = "drift_to" in fx
    snap_id = "s01"
    anchored, snap_digests = render_anchored(initial, snap_id)

    if proto == "str_replace":
        messages = [{"role": "system", "content": SR_SYS},
                    {"role": "user", "content": f"File:\n---\n{initial}---\nRewrite line "
                                                 f"{fx['target_line']} so the file becomes exactly the "
                                                 f"intended result. New content for that region: "
                                                 f"{fx['new_text']!r}"}]
    else:
        messages = [{"role": "system", "content": HL_SYS},
                    {"role": "user", "content": f"{anchored}\n\nRewrite line {fx['target_line']} "
                                                 f"to: {fx['new_text']!r}"}]

    tokens = 0.0
    for turn in range(1, max_turns + 1):
        if mock:
            content = _oracle(fx, proto, turn)
            toks = len(content) / 4.0
        else:
            assert endpoint
            try:
                content, toks = chat(messages, endpoint, model)
            except Exception as ex:
                return {"success": False, "turns": turn, "tokens": tokens, "note": f"call failed: {ex}"}
        tokens += toks
        edit = parse_json(content)

        if proto == "str_replace":
            result, err = apply_str_replace(current, edit.get("old_string", ""), edit.get("new_string", ""))
        else:
            result, err = apply_hashline(current, snap_digests, snap_id, edit.get("edits", []))

        # success: file lands on expected. Drift's safe first-move is a reject that
        # then recovers -- so for drift we require the model to ultimately produce
        # the expected file (after re-anchoring), not merely to fail.
        if result is not None and result == expected:
            return {"success": True, "turns": turn, "tokens": tokens, "note": "applied"}
        if result is not None and is_drift and result != expected:
            err = "error: that edit did not produce the intended result"

        # feed the real tool error back + a refreshed view, and let it retry
        messages.append({"role": "assistant", "content": content})
        if proto == "str_replace":
            view = current  # re-show the (possibly drifted) file
            messages.append({"role": "user", "content": f"Tool result: {err}\nCurrent file:\n---\n{view}---\nTry again."})
        else:
            re_anchored, snap_digests2 = render_anchored(current, snap_id + "b")
            snap_digests, snap_id = snap_digests2, snap_id + "b"
            messages.append({"role": "user", "content": f"Tool result: {err}\nRe-anchored file:\n{re_anchored}\nTry again."})

    return {"success": False, "turns": max_turns, "tokens": tokens, "note": "max turns"}


def _oracle(fx: Dict[str, Any], proto: str, turn: int) -> str:
    """Mock agent: on the ambiguous/drift fixtures it fumbles turn 1 the way a real
    weak model does, then recovers -- so --mock exercises the retry loop."""
    src = split_lines(fx["initial"])
    cur = split_lines(fx.get("drift_to", fx["initial"]))
    o, e = fx["target_line"], fx.get("end_line", fx["target_line"])
    if proto == "str_replace":
        if "drift_to" in fx:
            # turn 1 uses the stale line (fails not-found), turn 2 edits the drifted file
            if turn == 1:
                return json.dumps({"old_string": src[o - 1], "new_string": fx["new_text"]})
            # recover: no clean target on the drifted file -> reproduce expected via full replace
            return json.dumps({"old_string": fx.get("drift_to"), "new_string": fx["expected"]})
        if fx["category"] == "collision" and turn == 1:
            return json.dumps({"old_string": src[o - 1], "new_string": fx["new_text"]})  # ambiguous
        old = "\n".join(src[o - 1: e]) if e > o else src[o - 1]
        if fx["category"] == "collision":  # add context on retry
            old = "\n".join(src[o - 2: e + 1])
            new = "\n".join([src[o - 2]] + fx["new_text"].split("\n") + src[e: e + 1])
            return json.dumps({"old_string": old, "new_string": new})
        return json.dumps({"old_string": old, "new_string": fx["new_text"]})
    # hashline oracle: always correct first try (server owns the hard parts)
    _, digs = render_anchored(fx["initial"], "s01")
    if "drift_to" in fx:
        # turn 1 cites the stale anchor (stale_anchor), turn 2 re-anchors on the drift
        if turn == 1:
            return json.dumps({"snapshot_id": "s01", "edits": [{"op": "replace", "at": f"{o}:{short_tag(digs[o-1])}", "text": fx["new_text"]}]})
        cur_digs = [line_digest(l, i == 0) for i, l in enumerate(cur)]
        # after drift the target may not exist; reproduce expected by replacing all
        return json.dumps({"snapshot_id": "s01b", "edits": [{"op": "replace_range", "from": f"1:{short_tag(cur_digs[0])}", "to": f"{len(cur)}:{short_tag(cur_digs[-1])}", "text": fx["expected"].rstrip(chr(10))}]})
    if e > o:
        return json.dumps({"snapshot_id": "s01", "edits": [{"op": "replace_range", "from": f"{o}:{short_tag(digs[o-1])}", "to": f"{e}:{short_tag(digs[e-1])}", "text": fx["new_text"]}]})
    return json.dumps({"snapshot_id": "s01", "edits": [{"op": "replace", "at": f"{o}:{short_tag(digs[o-1])}", "text": fx["new_text"]}]})


def run(models: List[str], endpoint: Optional[str], runs: int, max_turns: int,
        fixtures: List[Dict[str, Any]], mock: bool) -> int:
    roster = models if models else (["oracle"] if mock else [])
    if not roster:
        print("no models: pass --mock or --model NAME --endpoint URL", file=sys.stderr)
        return 2

    print(f"hashline AGENTIC eval  ({len(fixtures)} fixtures x {len(roster)} model(s) x {runs} run(s), "
          f"max {max_turns} turns)\n")
    print(f"  {'model':<22} {'proto':<12} {'pass@1':>6} {'pass@k':>6} {'turns':>6} {'tokens':>7}")
    print("  " + "-" * 62)
    verdict_ok = True
    for model in roster:
        row = {}
        for proto in ("str_replace", "hashline"):
            p1 = pk = 0
            n = tot_turns = 0
            tot_tokens = 0.0
            for _ in range(runs):
                for fx in fixtures:
                    r = run_task(fx, proto, model, endpoint, max_turns, mock or model == "oracle")
                    n += 1
                    pk += int(r["success"])
                    p1 += int(r["success"] and r["turns"] == 1)
                    tot_turns += r["turns"]
                    tot_tokens += r["tokens"]
            row[proto] = (p1 / n, pk / n, tot_turns / n, tot_tokens / n)
            a, b, c, d = row[proto]
            print(f"  {model:<22} {proto:<12} {a:>6.0%} {b:>6.0%} {c:>6.2f} {d:>7.0f}")
        # the proposal's real gate: equal-or-better pass@k at fewer-or-equal turns
        _, sk, st, stok = row["str_replace"]
        _, hk, ht, htok = row["hashline"]
        better = hk >= sk - 1e-9 and ht <= st + 1e-9
        print(f"  -> {model}: hashline pass@k {hk:.0%} vs {sk:.0%}, turns {ht:.2f} vs {st:.2f}, "
              f"tokens {htok:.0f} vs {stok:.0f}  [{'OK' if better else 'REGRESSION'}]\n")
        verdict_ok = verdict_ok and better

    print("Gate: hashline pass@k >= str_replace at <= turns on every model -> "
          + ("PASS" if verdict_ok else "FAIL"))
    return 0 if verdict_ok else 1


def main() -> int:
    ap = argparse.ArgumentParser(description="hashline multi-turn agentic eval")
    ap.add_argument("--fixtures", type=Path, default=DEFAULT_FIXTURES)
    ap.add_argument("--endpoint")
    ap.add_argument("--model", action="append", default=[])
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("--max-turns", type=int, default=4)
    ap.add_argument("--mock", action="store_true")
    ap.add_argument("--token")
    ap.add_argument("--cacert", type=Path)
    ap.add_argument("--insecure", action="store_true")
    args = ap.parse_args()
    if not args.mock and not args.endpoint:
        print("error: pass --mock or --endpoint URL --model NAME", file=sys.stderr)
        return 2
    global _TLS_CTX, _BEARER
    _BEARER = args.token
    if args.insecure:
        _TLS_CTX = ssl._create_unverified_context()
    elif args.cacert:
        _TLS_CTX = ssl.create_default_context(cafile=str(args.cacert))
    fixtures = json.loads(args.fixtures.read_text())["fixtures"]
    return run(args.model, args.endpoint, args.runs, args.max_turns, fixtures, args.mock)


if __name__ == "__main__":
    sys.exit(main())
