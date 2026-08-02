"""Compare two runs of the same notes: did the outputs change, and how fast.

Used to decide whether a throughput change (parallel slots, a draft head, a
different server build) is free or whether it costs output fidelity. Both
questions matter and they are answered separately, because a speedup that
changes answers is not the same product as a speedup that does not.

Fidelity is judged on the RAW completion, byte for byte. Comparing parsed
triples would hide a real change behind the scorer's tolerance, and the point is
to detect drift, not to forgive it.
"""

import argparse
import json
import statistics


def load(p):
    return {json.loads(l)["id"]: json.loads(l) for l in open(p)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--a-label", default="A")
    ap.add_argument("--b-label", default="B")
    args = ap.parse_args()

    A, B = load(args.a), load(args.b)
    ids = [i for i in A if i in B]
    print(f"\ncomparing {len(ids)} notes present in both runs "
          f"(A={len(A)}, B={len(B)})\n")

    def stats(d, label):
        rows = [d[i] for i in ids]
        lat = [r["latency_ms"] for r in rows]
        tok = [r.get("completion_tokens") or 0 for r in rows]
        # Wall clock is not sum(latency) when requests overlap, so per-request
        # latency and aggregate throughput are reported separately: under
        # concurrency each request gets SLOWER while the run gets faster.
        print(f"{label}")
        print(f"   median latency/request {statistics.median(lat):8.0f} ms")
        print(f"   median gen tokens      {statistics.median(tok):8.0f}")
        print(f"   sum of request time    {sum(lat)/60000:8.1f} min")
        return sum(lat), statistics.median(lat)

    sa, ma = stats(A, args.a_label)
    sb, mb = stats(B, args.b_label)
    print(f"\n   per-request latency ratio  {mb/ma:.2f}x "
          f"({'slower' if mb > ma else 'faster'} per request)")

    same = sum(1 for i in ids if A[i]["raw"] == B[i]["raw"])
    toks = sum(1 for i in ids
               if (A[i].get("completion_tokens") or 0) != (B[i].get("completion_tokens") or 0))
    print(f"\nfidelity")
    print(f"   identical raw completions {same}/{len(ids)}  ({100*same/max(len(ids),1):.1f}%)")
    print(f"   differing token counts    {toks}/{len(ids)}")

    # A changed answer only matters if it changes the extracted facts, so that is
    # counted too -- but reported ALONGSIDE raw identity, never instead of it.
    def triples(r):
        return {(str(f.get("subject", "")).strip().lower(),
                 str(f.get("relation", "")).strip().lower(),
                 str(f.get("object", "")).strip().lower())
                for f in (r.get("pred_nofloor") or [])}

    tsame = sum(1 for i in ids if triples(A[i]) == triples(B[i]))
    print(f"   identical extracted triples {tsame}/{len(ids)}  "
          f"({100*tsame/max(len(ids),1):.1f}%)")

    shown = 0
    for i in ids:
        if A[i]["raw"] != B[i]["raw"] and shown < 3:
            shown += 1
            print(f"\n   DIFF {i}")
            print(f"     {args.a_label}: {A[i]['raw'][:150]}")
            print(f"     {args.b_label}: {B[i]['raw'][:150]}")

    print()
    if same == len(ids):
        print("VERDICT: byte-identical on every note. The speedup is free.")
    elif tsame == len(ids):
        print("VERDICT: raw text differs but every extracted triple is identical. "
              "Scores would be unchanged; provenance still differs.")
    else:
        print(f"VERDICT: {len(ids)-tsame} note(s) extract DIFFERENT facts. Runs using "
              "this configuration are not comparable to runs that do not.")


if __name__ == "__main__":
    main()
