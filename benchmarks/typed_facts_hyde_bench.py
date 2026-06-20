#!/usr/bin/env python3
"""Before/after benchmark for typed_facts (answer accuracy) and HyDE (retrieval
recall) against a live aimee /v1 server, using LoCoMo Cat-1 (single-hop factual)
questions.

The config flags (typed_facts_enabled, memory_rewrite_enabled/_hyde) are toggled
EXTERNALLY between the OFF and ON runs; this script only ingests and measures.

  ingest  : store the sample's conversation turns as memories (keyed bench_locomo_<sample>_<dia_id>),
            recording dia_id -> memory_id into a map file. Idempotent-ish (re-run overwrites map).
  hyde    : for each Cat-1 question, /v1/memory/search and score recall@5/@10 + MRR of the
            gold-evidence memory (find_facts reflects HyDE).
  answers : for each Cat-1 question, run a /v1/messages turn (the turn pipeline auto-injects the
            typed-fact "Known facts" block when typed_facts is on) and LLM-judge the answer vs gold.

Numbers are written to --out as JSON and printed.
"""
import argparse
import json
import sys
import time
import urllib.request

BASE = "http://192.168.1.254:8740"
BEARER = "aimee-local-dev"
MODEL = "claude-sonnet-4-6"
KEY_PREFIX = "bench_locomo_"
MAP_FILE = "/tmp/bench_locomo_map.json"
DATASET = "data/locomo/locomo10.json"


def api(path, body, timeout=15):
    data = json.dumps(body).encode()
    req = urllib.request.Request(
        BASE + path, data=data,
        headers={"Authorization": "Bearer " + BEARER, "Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read().decode())


def load_cases(sample_idx):
    d = json.load(open(DATASET))
    s = d[sample_idx]
    conv = s["conversation"]
    turns = {}  # dia_id -> "speaker: text"
    for k, v in conv.items():
        if isinstance(v, list):
            for t in v:
                if isinstance(t, dict) and "dia_id" in t:
                    turns[t["dia_id"]] = "%s: %s" % (t.get("speaker", ""), t.get("text", ""))
    cases = []
    for q in s["qa"]:
        if str(q.get("category")) != "1":
            continue
        ev = q.get("evidence", "[]")
        try:
            ev_ids = eval(ev) if isinstance(ev, str) else ev
        except Exception:
            ev_ids = []
        ev_ids = [e for e in ev_ids if e in turns]
        if ev_ids:
            cases.append({"q": q["question"], "a": str(q["answer"]), "ev": ev_ids})
    return turns, cases


def cmd_ingest(args):
    turns, cases = load_cases(args.sample)
    used = set()
    for c in cases:
        used.update(c["ev"])
    # ingest all turns of the sample (context), not just evidence, so retrieval is realistic
    dmap = {}
    n = 0
    for dia_id, text in turns.items():
        if args.max_turns and n >= args.max_turns:
            break
        key = "%s%d_%s" % (KEY_PREFIX, args.sample, dia_id.replace(":", "_"))
        try:
            r = api("/v1/memory/store", {"kind": "episode", "key": key, "content": text})
            if r.get("status") == "ok":
                dmap[dia_id] = r["id"]
                n += 1
        except Exception as e:
            print("store skip %s: %s" % (dia_id, e), file=sys.stderr, flush=True)
        if n % 10 == 0 and n:
            print("  ...stored %d" % n, flush=True)
    json.dump({"sample": args.sample, "map": dmap}, open(MAP_FILE, "w"))
    print("ingested %d turns; %d Cat-1 questions; map -> %s" % (n, len(cases), MAP_FILE))


def cmd_hyde(args):
    # Recall scored by CONTENT match (the gold-evidence turn text) so no id-map is
    # needed: we stored each turn verbatim, so a retrieved fact's content equals
    # the gold turn text when the right memory is surfaced.
    turns, cases = load_cases(args.sample)
    cases = cases[: args.max_q] if args.max_q else cases
    r5 = r10 = mrr = 0.0
    n = 0
    rows = []
    for c in cases:
        gold_texts = [turns[d] for d in c["ev"] if d in turns]
        if not gold_texts:
            continue
        try:
            resp = api("/v1/memory/search", {"keywords": [c["q"]]}, timeout=60)
        except Exception as e:
            print("search failed: %s" % e, file=sys.stderr, flush=True)
            continue
        n += 1
        contents = [f.get("content", "") for f in resp.get("facts", [])]
        rank = next((k + 1 for k, ct in enumerate(contents)
                     if any(g and (g == ct or g in ct or ct in g) for g in gold_texts)), 0)
        r5 += 1 if (rank and rank <= 5) else 0
        r10 += 1 if (rank and rank <= 10) else 0
        mrr += (1.0 / rank) if rank else 0.0
        rows.append({"q": c["q"], "rank": rank})
    res = {"mode": "hyde", "n": n, "recall@5": round(r5 / n, 4) if n else 0,
           "recall@10": round(r10 / n, 4) if n else 0, "mrr": round(mrr / n, 4) if n else 0,
           "rows": rows}
    json.dump(res, open(args.out, "w"))
    print(json.dumps({k: v for k, v in res.items() if k != "rows"}))


ANSWER_SYS = ("Answer the question in a few words using only what you know about the people "
              "mentioned. If you do not know, say 'unknown'. Be terse.")


def judge(question, gold, answer):
    prompt = ("Question: %s\nGold answer: %s\nCandidate answer: %s\n"
              "Does the candidate answer convey the same fact as the gold answer? "
              "Reply with exactly YES or NO." % (question, gold, answer))
    try:
        r = api("/v1/messages", {"model": MODEL, "max_tokens": 8,
                                 "messages": [{"role": "user", "content": prompt}]}, timeout=60)
        txt = "".join(b.get("text", "") for b in r.get("content", [])).strip().upper()
        return txt.startswith("YES")
    except Exception as e:
        print("judge failed: %s" % e, file=sys.stderr)
        return False


def cmd_answers(args):
    turns, cases = load_cases(args.sample)
    cases = cases[: args.max_q] if args.max_q else cases
    correct = 0
    rows = []
    for c in cases:
        try:
            r = api("/v1/messages", {"model": MODEL, "max_tokens": 64,
                                     "system": ANSWER_SYS,
                                     "messages": [{"role": "user", "content": c["q"]}]}, timeout=90)
            ans = "".join(b.get("text", "") for b in r.get("content", [])).strip()
        except Exception as e:
            print("answer failed: %s" % e, file=sys.stderr)
            ans = ""
        ok = judge(c["q"], c["a"], ans) if ans else False
        correct += 1 if ok else 0
        rows.append({"q": c["q"], "gold": c["a"], "answer": ans, "correct": ok})
        time.sleep(0.1)
    n = len(cases)
    res = {"mode": "answers", "n": n, "accuracy": round(correct / n, 4) if n else 0, "rows": rows}
    json.dump(res, open(args.out, "w"))
    print(json.dumps({k: v for k, v in res.items() if k != "rows"}))


def main():
    p = argparse.ArgumentParser()
    sub = p.add_subparsers(dest="cmd", required=True)
    pi = sub.add_parser("ingest"); pi.add_argument("--sample", type=int, default=0); pi.add_argument("--max-turns", type=int, default=0)
    ph = sub.add_parser("hyde"); ph.add_argument("--sample", type=int, default=0); ph.add_argument("--max-q", type=int, default=0); ph.add_argument("--out", default="/tmp/hyde.json")
    pa = sub.add_parser("answers"); pa.add_argument("--sample", type=int, default=0); pa.add_argument("--max-q", type=int, default=0); pa.add_argument("--out", default="/tmp/answers.json")
    args = p.parse_args()
    {"ingest": cmd_ingest, "hyde": cmd_hyde, "answers": cmd_answers}[args.cmd](args)


if __name__ == "__main__":
    main()
