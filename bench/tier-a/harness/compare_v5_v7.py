"""The v5-vs-v7 comparison, on one corpus, with an interval on the delta.

Every earlier prompt comparison in this effort was confounded. v5's numbers came
from a slice of gold_large and v6's from gold_small -- different notes of
different difficulty -- so the two were never comparable and the gap between them
said as much about the corpus as about the prompt. This scores both arms on the
same 1001 notes, from the same server, same quant, same session.

v7 emits retractions that the gold (written under the v1-v5 policy) labels EMPTY,
so its retraction facts are stripped before scoring. That is not a thumb on the
scale: on non-retraction notes both prompts want exactly the same output, which
is the comparison being made. Retraction quality is a separate measurement and
analyze_v6.py does it.
"""

import argparse
import json
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).parent


def strip_negated(src, dst):
    """Drop negated facts, keeping the run_llamacpp row shape."""
    n_dropped = 0
    with open(dst, "w") as out:
        for line in open(src):
            r = json.loads(line)
            raw = r.get("raw") or ""
            keep = []
            try:
                obj = json.loads(raw[raw.index("{"):raw.rindex("}") + 1])
                for f in obj.get("facts", []):
                    if isinstance(f, dict) and f.get("negated") is not True:
                        keep.append({"subject": f.get("subject", ""),
                                     "relation": f.get("relation", ""),
                                     "object": f.get("object", ""),
                                     "confidence": f.get("confidence", 0.0) or 0.0})
                    elif isinstance(f, dict):
                        n_dropped += 1
            except Exception:  # noqa: BLE001 - unparseable rows score as-is
                keep = r.get("pred_nofloor") or []
            r["pred_nofloor"] = keep
            r["pred"] = keep
            out.write(json.dumps(r, ensure_ascii=False) + "\n")
    return n_dropped


def score(gold, pred, out_json):
    subprocess.run([sys.executable, str(HERE / "score.py"), "--gold", gold,
                    "--pred", pred, "--json-out", out_json],
                   check=True, stdout=subprocess.DEVNULL)
    return json.load(open(out_json))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gold", required=True)
    ap.add_argument("--v5", required=True)
    ap.add_argument("--v7", required=True)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args()
    out = pathlib.Path(args.outdir)
    out.mkdir(parents=True, exist_ok=True)

    v7_stripped = str(out / "v7.asserted_only.pred.jsonl")
    dropped = strip_negated(args.v7, v7_stripped)
    print(f"stripped {dropped} negated facts from the v7 arm "
          f"(the gold has no notion of them)\n")

    s5 = score(args.gold, args.v5, str(out / "v5.score.json"))
    s7 = score(args.gold, v7_stripped, str(out / "v7.score.json"))

    print(f"{'view':20s} {'v5':>9s} {'v7':>9s} {'delta':>9s}")
    for view in ("strict", "lenient", "relation_agnostic"):
        a, b = s5[view]["f1"], s7[view]["f1"]
        print(f"{view:20s} {a:9.4f} {b:9.4f} {b - a:+9.4f}")
    for view in ("strict", "relation_agnostic"):
        for m in ("precision", "recall"):
            a, b = s5[view][m], s7[view][m]
            print(f"{view[:12] + '.' + m:20s} {a:9.4f} {b:9.4f} {b - a:+9.4f}")
    for label, s in (("v5", s5), ("v7", s7)):
        oe = s["over_extraction"]
        print(f"  {label}: abstention {oe['abstention_rate_on_schema']:.4f}  "
              f"spurious {oe['spurious_triples']}  "
              f"fabrication {s['fabrication']['fabrication_rate']}")

    print("\npaired bootstrap on the same notes:")
    subprocess.run([sys.executable, str(HERE / "bootstrap_ci.py"), "--gold", args.gold,
                    f"--pred", f"v5={args.v5}", "--pred", f"v7={v7_stripped}"], check=False)


if __name__ == "__main__":
    main()
