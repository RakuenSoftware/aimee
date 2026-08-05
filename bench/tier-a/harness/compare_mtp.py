"""What does MTP buy in throughput, and what does it cost in accuracy?

Speculative decoding is meant to be output-preserving: the draft is verified
against the target model, so accepted tokens are the ones the target would have
produced. If that holds exactly, MTP is free speed and the F1 delta is zero. This
prints both sides of that trade for a pair of arms that differ ONLY in DRAFT.

Throughput is reported three ways because they answer different questions, and
because one of them lies on short runs:

  per-stream tok/s   completion_tokens / latency_ms for each request, medianed.
                     This is what speculative decoding actually accelerates --
                     one request's decode loop. Use this to judge MTP itself.

  steady-state       nproc * 60 / median_latency_s. Throughput once the servers
  notes/min          are up, with startup excluded by construction. This is the
                     number to compare ACROSS configurations.

  wall-clock         rows / wall seconds. What the run actually cost, INCLUDING
  notes/min          server startup.

Wall clock is reported but must not be used for cross-configuration comparison.
Startup is roughly 30s per server, so it grows with process count -- 56s at
nproc=1 against 137s at nproc=4 on the 5080. On a short run that is a third to a
half of the wall clock, and the bias points the same way as any hypothesis about
process count. That is defect 35: it produced two confident wrong conclusions
(that aggregate throughput peaks at nproc=2 and declines, and that nproc=4 is
slower than nproc=1) before anyone checked what the denominator contained.

The gap between steady-state and wall-clock is printed as `startup` so it stays
visible rather than being absorbed into the result.

completion_tokens counts ACCEPTED tokens, not drafted ones, so neither figure
credits MTP for speculation that was rejected. That is the honest accounting: a
rejected draft costs time and produces nothing.

The trade line divides throughput gain by F1 cost. It is deliberately not
reported when the F1 delta is within noise -- a ratio against a denominator
indistinguishable from zero is not a number, and this benchmark's own history
(defect 32) is a case of exactly that being quoted for months.

Usage:
  python3 harness/compare_mtp.py --gold data/corpora/v5/gold_large.jsonl \\
      --mtp results/10k-sharded/E2B.UD-Q4_K_XL.10k \\
      --nomtp results/10k-nomtp/E2B.UD-Q4_K_XL.10k
  (paths are the arm stem: <stem>.pred.jsonl and <stem>.score.json)
"""

import argparse
import json
import os
import re
import statistics
import sys


def load_rows(stem):
    p = stem + ".pred.jsonl"
    return [json.loads(l) for l in open(p) if l.strip()]


def load_f1(stem):
    p = stem + ".score.json"
    if not os.path.exists(p):
        return None
    try:
        return json.load(open(p))["strict"]["f1"]
    except (KeyError, json.JSONDecodeError):
        return None


def wall_seconds(stem):
    """Parse the arm's wall clock out of its shard log DONE line.

    Returns None rather than guessing when the log is absent -- an aggregate
    rate computed against a made-up denominator is worse than no rate.
    """
    d, base = os.path.split(stem)
    log = os.path.join(d, "shard_%s.log" % base)
    if not os.path.exists(log):
        return None, None
    wall, procs = None, None
    for line in open(log):
        m = re.search(r"wall=(?:(\d+)m)?(?:(\d+)s)?\s+procs=(\d+)", line)
        if m:
            mins = int(m.group(1) or 0)
            secs = int(m.group(2) or 0)
            wall = mins * 60 + secs
            procs = int(m.group(3))
    return wall, procs


def per_stream_toks(rows):
    rates = []
    for r in rows:
        ct, lat = r.get("completion_tokens"), r.get("latency_ms")
        if ct and lat:
            rates.append(ct / (lat / 1000.0))
    return statistics.median(rates) if rates else None


def summarise(stem):
    rows = load_rows(stem)
    wall, procs = wall_seconds(stem)
    total_ct = sum(r.get("completion_tokens") or 0 for r in rows)
    med_lat = statistics.median([r.get("latency_ms") or 0 for r in rows]) if rows else 0
    # Steady state excludes startup by construction: it asks how fast the servers
    # go once they are up, not how long the job took door to door.
    steady = (procs * 60 / (med_lat / 1000.0)) if (procs and med_lat) else None
    wallclock = (len(rows) * 60 / wall) if wall else None
    # Seconds of the wall clock not accounted for by generation at steady state.
    startup = None
    if steady and wall:
        startup = wall - (len(rows) / (steady / 60.0))
    return {
        "stem": stem,
        "n": len(rows),
        "f1": load_f1(stem),
        "per_stream": per_stream_toks(rows),
        "total_tokens": total_ct,
        "wall_s": wall,
        "procs": procs,
        "steady_npm": steady,
        "wallclock_npm": wallclock,
        "startup_s": startup,
        "median_ct": statistics.median([r.get("completion_tokens") or 0 for r in rows]) if rows else 0,
        "median_lat": med_lat,
    }


def pct(new, old):
    if not old:
        return None
    return (new - old) / old * 100.0


def fmt(v, spec=".2f"):
    return "n/a" if v is None else format(v, spec)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mtp", required=True, help="arm stem for the MTP side")
    ap.add_argument("--nomtp", required=True, help="arm stem for the no-MTP side")
    ap.add_argument("--gold", help="unused; accepted so the call site reads symmetrically")
    # DEFECT 39. This used to default to 0.0105 and call it a "noise threshold".
    # 0.0105 is finding 19's MEASURED EFFECT of process count on F1, from an
    # unrelated experiment. It is not an interval and it never was, and using it
    # as one made every ranking gap between 0.0105 and the real interval look
    # settled when it was not.
    #
    # There is no correct constant to put here, which is the point: the interval
    # depends on n and on the pair. So this now only decides whether to PRINT a
    # ratio, it makes no claim about significance, and the real answer comes from
    # harness/bootstrap_ci.py on the same two arms. The default is the rough
    # +/-0.024 that n=1001 supports, which is deliberately conservative at
    # n=10000 -- it suppresses a ratio more often than needed rather than
    # asserting a trade that a bootstrap would refuse.
    ap.add_argument("--noise", type=float, default=0.024,
                    help="F1 delta below which no gain-per-point ratio is printed. "
                         "NOT a significance threshold: run bootstrap_ci.py for that.")
    args = ap.parse_args()

    a, b = summarise(args.mtp), summarise(args.nomtp)

    if a["n"] != b["n"]:
        print(f"WARNING: row counts differ ({a['n']} vs {b['n']}); "
              f"the pair is not comparable.", file=sys.stderr)

    print(f"{'':22} {'MTP':>14} {'no-MTP':>14} {'delta':>14}")
    print(f"{'rows':22} {a['n']:>14} {b['n']:>14}")
    print(f"{'strict F1':22} {fmt(a['f1'], '.4f'):>14} {fmt(b['f1'], '.4f'):>14} "
          f"{fmt((a['f1'] - b['f1']) if (a['f1'] and b['f1']) else None, '+.4f'):>14}")
    print(f"{'per-stream tok/s':22} {fmt(a['per_stream']):>14} {fmt(b['per_stream']):>14} "
          f"{fmt(pct(a['per_stream'], b['per_stream']), '+.1f') + '%':>14}")
    print(f"{'steady notes/min':22} {fmt(a['steady_npm']):>14} {fmt(b['steady_npm']):>14} "
          f"{fmt(pct(a['steady_npm'], b['steady_npm']), '+.1f') + '%':>14}   <- compare on this")
    print(f"{'wall-clock notes/min':22} {fmt(a['wallclock_npm']):>14} {fmt(b['wallclock_npm']):>14} "
          f"{fmt(pct(a['wallclock_npm'], b['wallclock_npm']), '+.1f') + '%':>14}   (includes startup)")
    print(f"{'startup (s)':22} {fmt(a['startup_s'], '.0f'):>14} {fmt(b['startup_s'], '.0f'):>14}")
    print(f"{'wall (s)':22} {fmt(a['wall_s'], '.0f'):>14} {fmt(b['wall_s'], '.0f'):>14} "
          f"{fmt(pct(a['wall_s'], b['wall_s']), '+.1f') + '%':>14}")
    print(f"{'median completion tok':22} {a['median_ct']:>14} {b['median_ct']:>14}")
    print(f"{'median latency (ms)':22} {fmt(a['median_lat'], '.1f'):>14} {fmt(b['median_lat'], '.1f'):>14}")
    print(f"{'processes':22} {fmt(a['procs'], 'd'):>14} {fmt(b['procs'], 'd'):>14}")

    print()
    if a["f1"] is None or b["f1"] is None:
        print("F1 missing on one side; no trade computed.")
        return
    d = a["f1"] - b["f1"]
    gain = pct(a["steady_npm"], b["steady_npm"])
    if gain is None:
        gain = pct(a["per_stream"], b["per_stream"])
    print(f"MTP costs {(-d):+.4f} F1 and changes steady-state throughput by "
          f"{fmt(gain, '+.1f')}%.")
    if abs(d) < args.noise:
        print(f"|delta| is under {args.noise}, so no gain-per-point ratio is "
              f"printed: dividing by a denominator that may be zero manufactures a "
              f"number rather than measuring one. This is a display rule, NOT a "
              f"significance test -- run harness/bootstrap_ci.py on these two arms "
              f"for the interval, per defect 39.")
    elif gain is not None:
        print(f"Trade: {gain / (abs(d) * 100):.2f}% throughput per 0.01 F1 given up."
              f"  ({'MTP is worse on accuracy' if d < 0 else 'MTP is better on accuracy'})")


if __name__ == "__main__":
    main()
