#!/usr/bin/env python3
"""Per-pair bootstrap intervals for every ordering claim in the head-to-head.

Draft 00 states its whole table at +/-0.024, the rough interval n=1001 supports,
with no per-pair interval behind any specific claim. Defect 39 is exactly that
mistake one level up: a single number standing in for intervals it never earned.

This runs a paired bootstrap on the comparisons the article actually makes:
every adjacent pair in the ranking, which is what an ordering claim IS, plus the
named claims in the prose. Paired means the same notes are resampled for both
arms, so it prices the difference rather than the two arms separately.
"""
import json, os, subprocess, sys
# unbuffered: a 20-minute sweep that prints nothing until it ends is a sweep you
# cannot tell from a hung one, and I could not.
sys.stdout.reconfigure(line_buffering=True)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLD = os.path.join(ROOT, "data/corpora/v5/gold_small.jsonl")

ARMS = {
 "E2B-QAT":      "results/qat-vs-ud/gemma-4-E2B-it.qat",
 "E4B-QAT":      "results/qat-vs-ud/gemma-4-E4B-it.qat",
 "E4B-UD":       "results/v5-rerun-gguf/gemma-4-E4B-it",
 "E2B-UD":       "results/v5-rerun-gguf/gemma-4-E2B-it",
 "LFM2.5-2.6B-Q4": "results/lfm25-2.6b/LFM2.5-2.6B.Q4_K_M",
 "LFM2.5-2.6B-Q8": "results/lfm25-2.6b/LFM2.5-2.6B.Q8_0",
 "granite-4.1-3b": "results/v5-rerun-gguf/granite-4.1-3b",
 "gemma-3n-E4B":  "results/subset-1001/gemma-3n-E4B.sub1001",
 "LFM2.5-8B-A1B": "results/lfm25-8b/LFM2.5-8B-A1B.Q4_K_M",
 "Qwen3-1.7B":    "results/subset-1001/Qwen3-1.7B.sub1001",
 "SmolLM3-Q8":    "results/newcomers-1k/SmolLM3-3B.Q8_0",
 "granite-4.0-1b":"results/v5-rerun-gguf/granite-4.0-1b",
 "SmolLM3-Q4":    "results/newcomers-1k/SmolLM3-3B.Q4_K_M",
 "LFM2.5-VL-1.6B":"results/lfm25-family/LFM2.5-VL-1.6B.Q8_0",
 "LFM2.5-1.2B":   "results/lfm25-family/LFM2.5-1.2B-Instruct.Q8_0",
 "LFM2.5-230M":   "results/lfm25-family/LFM2.5-230M.Q8_0",
}

# Ranking order as draft 00 prints it, so "adjacent" means what the reader sees.
ORDER = ["E2B-QAT", "E4B-QAT", "E4B-UD", "E2B-UD", "LFM2.5-2.6B-Q4",
         "LFM2.5-2.6B-Q8", "granite-4.1-3b", "gemma-3n-E4B", "LFM2.5-8B-A1B",
         "Qwen3-1.7B", "SmolLM3-Q8", "granite-4.0-1b", "SmolLM3-Q4",
         "LFM2.5-VL-1.6B", "LFM2.5-1.2B", "LFM2.5-230M"]

NAMED = [("E2B-QAT", "E4B-UD",       "the top of the table beats E4B"),
         ("E2B-QAT", "E2B-UD",       "QAT vs UD on E2B (+0.0389 claimed)"),
         ("SmolLM3-Q8", "SmolLM3-Q4","more bits helps SmolLM3 (+0.0352)"),
         ("LFM2.5-2.6B-Q4", "LFM2.5-2.6B-Q8", "fewer bits helps LFM2.5-2.6B"),
         ("granite-4.1-3b", "gemma-3n-E4B", "the restraint pick vs the arm above it")]


def boot(a, b):
    cmd = [sys.executable, os.path.join(ROOT, "harness/bootstrap_ci.py"),
           "--gold", GOLD,
           "--pred", "%s=%s" % (a, os.path.join(ROOT, ARMS[a] + ".pred.jsonl")),
           "--pred", "%s=%s" % (b, os.path.join(ROOT, ARMS[b] + ".pred.jsonl")),
           "--boot", "8000"]
    r = subprocess.run(cmd, capture_output=True, text=True)
    # bootstrap_ci.py prints exactly one line of the form
    #   "B - A    -0.0390  [-0.0617,-0.0154]  significant"
    # Match on the leading comparison token rather than on punctuation, which is
    # what the first version got wrong: it guessed at the format and silently
    # returned nothing for every pair.
    want = "%s - %s" % (b, a)
    for ln in r.stdout.splitlines():
        t = " ".join(ln.split())
        if t.startswith(want):
            return t[len(want):].strip()
    return "PARSE FAILED rc=%d %s" % (r.returncode, (r.stderr or r.stdout)[-160:].replace("\n", " "))


def main():
    print("ADJACENT PAIRS -- each one is an ordering claim the table makes\n")
    flipped = same = 0
    for x, y in zip(ORDER, ORDER[1:]):
        if x not in ARMS or y not in ARMS:
            continue
        line = boot(x, y)
        verdict = "INDISTINGUISHABLE" in line
        same += verdict; flipped += not verdict
        print("  %-34s %s" % ("%s vs %s" % (x, y), line))
    print("\n  %d of %d adjacent pairs are separable; %d are not.\n" % (flipped, flipped + same, same))
    print("NAMED CLAIMS IN THE PROSE\n")
    for x, y, why in NAMED:
        print("  %-46s %s" % (why, boot(x, y)))


if __name__ == "__main__":
    main()
