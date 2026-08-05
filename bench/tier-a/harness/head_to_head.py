#!/usr/bin/env python3
"""The head-to-head this project was built to produce.

Every model with a native 1001-note arm on the current corpus (gold_small, v5),
scored on the columns that decide whether a model is usable rather than on F1
alone. Two arms are marked `extracted` because they were lifted out of a
10,000-note run rather than run natively at this tier; defect 40 measures that
difference at -0.0079, inside the interval, but it is not the same measurement.

Columns beyond F1, and why each is here:
  parse/schema  a model can score zero for answering in the wrong envelope
  abstention    rate of correctly saying nothing on the 322 factless notes
  spurious      triples invented on those same notes
  reason        fraction of rows where the model emitted a reasoning pass

No re-running. Scores banked predictions with the unmodified scorer.
"""
import json, subprocess, sys, os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLD = os.path.join(ROOT, "data/corpora/v5/gold_small.jsonl")
SCRATCH = os.path.join(ROOT, ".scratch")

ARMS = [
 ("gemma-4-E4B",    "UD-Q4",    "results/v5-rerun-gguf/gemma-4-E4B-it",         "native"),
 ("gemma-4-E2B",    "UD-Q4",    "results/v5-rerun-gguf/gemma-4-E2B-it",         "native"),
 ("gemma-4-E4B",    "QAT q4_0", "results/qat-vs-ud/gemma-4-E4B-it.qat",         "native"),
 ("gemma-4-E2B",    "QAT q4_0", "results/qat-vs-ud/gemma-4-E2B-it.qat",         "native"),
 ("granite-4.1-3b", "UD-Q4",    "results/v5-rerun-gguf/granite-4.1-3b",         "native"),
 ("granite-4.0-1b", "UD-Q4",    "results/v5-rerun-gguf/granite-4.0-1b",         "native"),
 ("gemma-3n-E4B",   "UD-Q4",    "results/subset-1001/gemma-3n-E4B.sub1001",     "extracted"),
 ("Qwen3-1.7B",     "UD-Q4",    "results/subset-1001/Qwen3-1.7B.sub1001",       "extracted"),
 ("SmolLM3-3B",     "Q8_0",     "results/newcomers-1k/SmolLM3-3B.Q8_0",         "native"),
 ("SmolLM3-3B",     "Q4_K_M",   "results/newcomers-1k/SmolLM3-3B.Q4_K_M",       "native"),
 ("MiniCPM5-1B",    "Q8_0",     "results/newcomers-1k/MiniCPM5-1B.Q8_0",        "native"),
 ("MiniCPM5-1B",    "Q4_K_M",   "results/newcomers-1k/MiniCPM5-1B.Q4_K_M",      "native"),
 ("LFM2.5-8B-A1B",  "Q4_K_M",   "results/lfm25-8b/LFM2.5-8B-A1B.Q4_K_M",        "nproc!=3"),
 ("LFM2.5-2.6B",    "Q8_0",     "results/lfm25-2.6b/LFM2.5-2.6B.Q8_0",          "native"),
 ("LFM2.5-2.6B",    "Q4_K_M",   "results/lfm25-2.6b/LFM2.5-2.6B.Q4_K_M",        "native"),
 ("LFM2.5-VL-1.6B", "Q8_0",     "results/lfm25-family/LFM2.5-VL-1.6B.Q8_0",     "native"),
 ("LFM2.5-1.2B",    "Q8_0",     "results/lfm25-family/LFM2.5-1.2B-Instruct.Q8_0","native"),
 ("LFM2.5-230M",    "Q8_0",     "results/lfm25-family/LFM2.5-230M.Q8_0",        "native"),
]


def score_path(stem):
    banked = os.path.join(ROOT, stem + ".score.json")
    if os.path.exists(banked):
        return banked
    out = os.path.join(SCRATCH, os.path.basename(stem) + ".score.json")
    cmd = [sys.executable, os.path.join(ROOT, "harness/score.py"),
           "--gold", GOLD, "--pred", os.path.join(ROOT, stem + ".pred.jsonl"),
           "--json-out", out]
    if subprocess.run(cmd, capture_output=True).returncode != 0:
        if subprocess.run(cmd + ["--allow-thinking-off"], capture_output=True).returncode != 0:
            return None
    return out


def main():
    os.makedirs(SCRATCH, exist_ok=True)
    print("%-16s %-9s %-10s %7s %7s %7s %6s %6s %6s %6s %6s" % (
        "model", "quant", "run", "F1", "prec", "rec", "parse", "schema",
        "abst", "spur", "reason"))
    for name, q, stem, kind in ARMS:
        pred = os.path.join(ROOT, stem + ".pred.jsonl")
        if not os.path.exists(pred):
            print("%-16s %-9s MISSING" % (name, q))
            continue
        sj = score_path(stem)
        if sj is None:
            print("%-16s %-9s BLOCKED by a scorer guard" % (name, q))
            continue
        d = json.load(open(sj))
        s = d["strict"]
        oe = d.get("over_extraction") or {}
        rows = [json.loads(l) for l in open(pred)]
        n = len(rows)
        pk = sum(1 for r in rows if r.get("parse_ok")) / n
        sk = sum(1 for r in rows if r.get("schema_ok")) / n
        rz = sum(1 for r in rows if (r.get("reasoning_chars") or 0) > 0) / n
        ab = oe.get("abstention_rate_on_schema")
        print("%-16s %-9s %-10s %7.4f %7.4f %7.4f %6.2f %6.2f %6s %6s %6.2f" % (
            name, q, kind, s["f1"], s["precision"], s["recall"], pk, sk,
            ("%.3f" % ab) if ab is not None else "-",
            oe.get("spurious_triples", "-"), rz))


if __name__ == "__main__":
    main()
