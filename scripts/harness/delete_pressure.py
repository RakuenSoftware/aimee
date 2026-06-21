#!/usr/bin/env python3
"""delete_pressure.py: rank harness scaffolding by how much it bets against the
model, so it can be deleted as models improve.

Premise (the "four-part harness"): every harness component encodes an assumption
about what the model can't do on its own. As models get better, a good harness
gets SMALLER, not bigger — the scaffolding that restated the obvious becomes a
pure context tax (a "token bonfire"). This tool puts a number on that pressure for
the per-tool prompt augmentations in src/tool_prompts/*.md — the most literal
"betting against the model" surface in aimee, injected into context on every turn.

Two signals, deliberately separated:

  * STATIC (always available, computed here): tax = how many tokens this scaffold
    costs every turn, and doubt-density = how prescriptive it is ("Always",
    "Never", "Avoid", "Do not"...). High tax + high doubt = strong candidate to
    re-test against a current model and likely delete.

  * RUNTIME (authoritative, needs instrumentation): did removing the scaffold
    change any outcome? Static signals only NOMINATE; the real verdict is an
    A/B/hit-rate measurement, which is the in-process counter specced in
    docs/proposals/pending/four-part-harness-taxonomy.md. This tool ingests that
    data when given it (--anti-patterns export.json: never-bumped patterns are
    removal candidates) and is explicit when it is missing rather than pretending
    the static score is the whole story.

Pure static analysis by default (no build, no runtime). Matches the check-*.py
house style. Run --self-test for a sanity check on synthetic inputs.

Usage:
  delete_pressure.py                      # scan src/tool_prompts/
  delete_pressure.py --dir src/tool_prompts --json
  delete_pressure.py --anti-patterns ap-export.json
  delete_pressure.py --self-test
"""
import argparse
import glob
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_DIR = os.path.normpath(os.path.join(HERE, "..", "..", "src", "tool_prompts"))

# Imperative hedges: each one encodes "the model won't do this unless told". The
# more of these per scaffold, the more it is compensating for a model weakness —
# exactly what should evaporate as models improve.
DOUBT_TERMS = (
    "always", "never", "avoid", "do not", "don't", "must", "only",
    "prefer", "ensure", "make sure", "be sure", "remember to", "instead of",
)

# Match whole words only. Bare substring counting over-counts badly ("only" inside
# "commonly", "must" inside "mustard", "prefer" inside "preferences"), which would
# inflate the doubt score of perfectly ordinary prose. Word boundaries are placed
# on the word-character edges, so multi-word and apostrophe terms still match.
DOUBT_RE = re.compile(
    r"\b(?:" + "|".join(re.escape(t) for t in DOUBT_TERMS) + r")\b")


# Rough token estimate without a tokenizer dep: ~0.75 words/token for English.
def est_tokens(text):
    words = len(re.findall(r"\S+", text))
    return max(1, round(words / 0.75))


def doubt_hits(text):
    return len(DOUBT_RE.findall(text.lower()))


def score_scaffold(name, text):
    text = text.strip()
    tokens = est_tokens(text)
    doubt = doubt_hits(text)
    words = len(re.findall(r"\S+", text))
    # Doubt density per 20 words, so a short prescriptive line scores like a long
    # one. Pressure blends per-turn tax with prescriptiveness.
    density = (doubt / words * 20) if words else 0.0
    pressure = round(tokens * (1.0 + density), 1)
    return {
        "name": name,
        "tokens_per_turn": tokens,
        "words": words,
        "doubt_terms": doubt,
        "doubt_density_per20w": round(density, 2),
        "pressure": pressure,
    }


def scan_dir(path):
    rows = []
    for fp in sorted(glob.glob(os.path.join(path, "*.md"))):
        name = os.path.splitext(os.path.basename(fp))[0]
        with open(fp, encoding="utf-8") as fh:
            rows.append(score_scaffold(name, fh.read()))
    return rows


def load_anti_patterns(path):
    """Optional runtime signal. Expects a JSON array of anti-pattern rows with at
    least {pattern, hit_count|bumps, last_seen?}. Never-bumped = removal candidate.
    Shape matches src/db2/anti_patterns.h (db2_anti_pattern_list output).

    Returns None on any load error (unreadable, bad JSON, not a JSON array) so the
    caller can fail loudly rather than silently fall back to static-only. A row that
    carries NONE of hit_count/bumps/hits gets hits=None ("unknown"), kept distinct
    from a genuine 0 — a schema mismatch must not turn every row into a bogus
    "never matched, delete it" recommendation."""
    try:
        with open(path, encoding="utf-8") as fh:
            data = json.load(fh)
    except (OSError, json.JSONDecodeError) as e:
        print(f"error: cannot read anti-patterns export {path}: {e}", file=sys.stderr)
        return None
    if not isinstance(data, list):
        print(f"error: anti-patterns export must be a JSON array, got "
              f"{type(data).__name__}", file=sys.stderr)
        return None
    out = []
    for ap in data:
        if not isinstance(ap, dict):
            continue
        hits = None
        for k in ("hit_count", "bumps", "hits"):
            if k in ap:
                hits = ap[k]
                break
        out.append({
            "pattern": ap.get("pattern", "?"),
            "hits": hits,
            "has_count": hits is not None,
            "source": ap.get("source", ""),
            "last_seen": ap.get("last_seen", ""),
        })
    return out


def render(rows, ap_rows):
    out = []
    total_tax = sum(r["tokens_per_turn"] for r in rows)
    out.append(f"Tool-prompt scaffolding: {len(rows)} scaffolds, "
               f"~{total_tax} tokens injected every turn.\n")
    name_w = max((len(r["name"]) for r in rows), default=4)
    out.append(f"  {'scaffold'.ljust(name_w)}  tok/turn  doubt  pressure")
    for r in sorted(rows, key=lambda x: -x["pressure"]):
        out.append(f"  {r['name'].ljust(name_w)}  {r['tokens_per_turn']:8d}  "
                   f"{r['doubt_terms']:5d}  {r['pressure']:8.1f}")
    out.append("")
    ranked = sorted(rows, key=lambda x: -x["pressure"])
    if ranked:
        out.append("Highest delete-pressure (re-test against a current model first):")
        for r in ranked[:3]:
            out.append(f"  - {r['name']}: {r['tokens_per_turn']} tok/turn, "
                       f"{r['doubt_terms']} prescriptive term(s)")
    out.append("")
    if ap_rows is None:
        out.append("Runtime signal: not provided. Static pressure only NOMINATES "
                   "scaffolds; the authoritative test is whether removing one changes "
                   "an outcome. Pass --anti-patterns <export.json> to fold in hit-rate, "
                   "or wire the in-process counter from the taxonomy proposal.")
    else:
        # Only a present-and-zero count makes a pattern a removal candidate. Rows
        # with no count field at all (hits is None) are malformed/schema-mismatched,
        # not "never matched" — surfacing them as such would recommend deleting live
        # patterns wholesale.
        dead = [a for a in ap_rows if a["hits"] == 0]
        unknown = [a for a in ap_rows if a["hits"] is None]
        out.append(f"Runtime signal: {len(ap_rows)} anti-patterns, "
                   f"{len(dead)} never matched (removal candidates):")
        for a in dead[:10]:
            out.append(f"  - never-hit: {a['pattern'][:70]}")
        if unknown:
            out.append(f"  ({len(unknown)} row(s) had no hit_count/bumps/hits field "
                       f"— excluded from removal candidates; check the export schema)")
    return "\n".join(out)


def _self_test():
    ok = True

    def check(name, cond):
        nonlocal ok
        if not cond:
            ok = False
        print(f"  [{'PASS' if cond else 'FAIL'}] {name}")

    # Length and prescriptiveness must not be confounded: hold word count fixed and
    # vary ONLY bossiness. The bossy one must score strictly higher on doubt and
    # pressure, proving the score reacts to prescriptiveness, not just length.
    plain = score_scaffold("plain", "Set the path here. List the directory once. Write the file.")
    bossy = score_scaffold("bossy", "Always set the path. Never list the directory. Do not write.")
    check(f"equal-length: bossy out-doubts plain "
          f"({plain['doubt_terms']} vs {bossy['doubt_terms']})",
          plain["words"] == bossy["words"] and bossy["doubt_terms"] > plain["doubt_terms"])
    check(f"equal-length: bossy out-pressures plain "
          f"({plain['pressure']} vs {bossy['pressure']})",
          bossy["pressure"] > plain["pressure"])
    check("plain prose registers zero doubt", plain["doubt_terms"] == 0)

    # Word-boundary matching: ordinary words that merely CONTAIN a doubt term must
    # not count. Without this guard 'commonly'/'mustard'/'preferences' would score.
    trap = score_scaffold("trap", "Commonly used mustard preferences are listed here.")
    check("substrings of doubt terms do not count (commonly/mustard/preferences)",
          trap["doubt_terms"] == 0)

    # Anti-pattern rows with NO count field must be 'unknown', not a removal
    # candidate: a schema mismatch must not recommend deleting live patterns. Only a
    # present-and-zero count counts as 'never matched'.
    rows = [score_scaffold("x", "noop")]
    ap_rows = [
        {"pattern": "real-zero", "hits": 0, "has_count": True},
        {"pattern": "schema-mismatch", "hits": None, "has_count": False},
    ]
    out = render(rows, ap_rows)
    check("present-zero count is a removal candidate", "never-hit: real-zero" in out)
    check("missing-count row is excluded, flagged as schema issue",
          "never-hit: schema-mismatch" not in out and "1 never matched" in out
          and "check the export schema" in out)

    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dir", default=DEFAULT_DIR,
                    help=f"directory of *.md scaffolds (default: {DEFAULT_DIR})")
    ap.add_argument("--anti-patterns", metavar="JSON",
                    help="optional anti-pattern export to fold in runtime hit-rate")
    ap.add_argument("--json", action="store_true", help="emit JSON")
    ap.add_argument("--self-test", action="store_true")
    args = ap.parse_args(argv)

    if args.self_test:
        return _self_test()

    if not os.path.isdir(args.dir):
        print(f"error: not a directory: {args.dir}", file=sys.stderr)
        return 2
    rows = scan_dir(args.dir)
    ap_rows = None
    if args.anti_patterns:
        ap_rows = load_anti_patterns(args.anti_patterns)
        if ap_rows is None:  # load printed the error already
            return 2

    if args.json:
        print(json.dumps({"scaffolds": rows, "anti_patterns": ap_rows}, indent=2))
    else:
        print(render(rows, ap_rows))
    return 0


if __name__ == "__main__":
    sys.exit(main())
