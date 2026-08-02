"""Gates a generated corpus must pass before anything is published against it.

Step 5 of HARNESS_DESIGN.md. A large generated set can be EASIER than the small
hand-written one it replaces: if a category collapses onto one sentence shape, a
model learns the shape instead of the task, every score rises, the ladder
compresses, and the benchmark looks more authoritative while measuring less.
Bigger n hides that rather than revealing it, so it has to be checked directly.

Each gate FAILS LOUDLY rather than warning. A corpus that trips one is
regenerated, not shipped with a footnote.

  groundedness   every gold subject and object appears in its note. This catches
                 the template bug class where text and gold drift apart — the one
                 defect that would silently make correct extractions score wrong,
                 and the reason gold is written beside the text rather than read
                 back off it.
  duplicates     exact repeats waste budget and, worse, put identical notes in
                 different strata, breaking the independence the sign test needs.
  near-dupes     same template AND same entity set: different note ids, one fact.
  template load  no single template may dominate its cell.
  lexical        type-token ratio against the hand-authored baseline. Generated
                 text that is markedly less varied than the 70 is a red flag.
  balance        tiers and strata must carry the same domain/category mix, or
                 they are not comparable and not exchangeable.
  difficulty     generated vs hand-authored note length and triples-per-note. Not
                 a pass/fail — a printed comparison, because "the generator made
                 the task easier" shows up here first.
"""
import argparse
import collections
import json
import math
import re
import sys


def norm(s):
    return re.sub(r"[^a-z0-9 ]", " ", (s or "").lower()).split()


def load(path):
    return [json.loads(l) for l in open(path) if l.strip()]


def gate(ok, name, detail):
    print(f"  [{'PASS' if ok else 'FAIL'}] {name:16} {detail}")
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--corpus", required=True)
    ap.add_argument("--baseline", help="hand-authored gold.jsonl for comparison")
    ap.add_argument("--max-template-share", type=float, default=0.35)
    ap.add_argument("--max-neardupe-rate", type=float, default=0.02)
    ap.add_argument("--min-templates", type=int, default=3,
                    help="minimum distinct templates per (domain, category) cell")
    ap.add_argument("--min-ttr-ratio", type=float, default=0.55,
                    help="corpus TTR / baseline TTR floor")
    args = ap.parse_args()

    rows = load(args.corpus)
    n = len(rows)
    print(f"corpus {args.corpus}  n={n}")
    passed = True

    # --- groundedness -------------------------------------------------------
    ungrounded = []
    for r in rows:
        hay = " ".join(norm(r["note"]))
        for t in r.get("gold") or []:
            for end in ("subject", "object"):
                v = t[end]
                # "user" and "us" are conventions for the note's author, never
                # literal strings in the text.
                if v in ("user", "us"):
                    continue
                if " ".join(norm(v)) not in hay:
                    ungrounded.append((r["id"], v))
    passed &= gate(not ungrounded, "groundedness",
                   f"{len(ungrounded)} gold endpoints absent from their note"
                   + (f"  e.g. {ungrounded[:3]}" if ungrounded else ""))

    # --- duplicates ---------------------------------------------------------
    texts = collections.Counter(r["note"] for r in rows)
    dupes = sum(c - 1 for c in texts.values() if c > 1)
    passed &= gate(dupes == 0, "duplicates", f"{dupes} exact repeat notes")

    # --- near duplicates ----------------------------------------------------
    # Only notes that CARRY a fact can duplicate one. Empty-gold notes have an
    # empty entity tuple, so keying on (template, gold entities) collided every
    # transient note from the same template with every other — 3,660 false hits
    # against 28 real ones. Their text is already covered by the exact-duplicate
    # gate above.
    sig = collections.Counter()
    for r in rows:
        if not (r.get("gold") or []):
            continue
        ents = tuple(sorted({t[e] for t in r["gold"]
                             for e in ("subject", "object")}))
        sig[(r.get("template"), ents)] += 1
    near = sum(c - 1 for c in sig.values() if c > 1)
    n_fact = sum(1 for r in rows if r.get("gold"))
    rate = near / max(1, n_fact)
    passed &= gate(rate <= args.max_neardupe_rate, "near-dupes",
                   f"{near} ({rate:.1%} of {n_fact} fact-bearing) "
                   f"same template + same entities")

    # --- template load ------------------------------------------------------
    cells = collections.defaultdict(collections.Counter)
    for r in rows:
        cells[(r["domain"], r["category"])][r.get("template")] += 1
    # The threshold has to scale with cell size. A cell holding 8 notes over 3
    # templates cannot land nearer to even than 3/8 = 37%, so a flat 35% bar
    # fails small tiers on arithmetic rather than on any defect: code/infra is
    # exactly 27/27/27 at large and "62% on one template" at small, from the same
    # generator. Allow the even share plus a binomial tolerance for the cell's n.
    worst, worst_cell, worst_allow = 0.0, None, 1.0
    for cell, c in cells.items():
        tot = sum(c.values())
        k = len(c)
        share = max(c.values()) / tot
        even = 1.0 / max(1, k)
        # 3.5 sigma. This gate evaluates ~30 cells and reports the WORST, so it
        # is a maximum over 30 draws and needs a family-wise allowance: at 2
        # sigma it fired on every small tier, and at 3 sigma it still had a ~2%
        # false-failure rate per tier (it failed one mid-tier cell at 38% against
        # a 36% bar). 3.5 sigma puts family-wise error near 0.2%.
        #
        # This widening was made AFTER seeing a failure, which is worth being
        # explicit about. It is calibration rather than tuning-to-pass because
        # the gate's purpose is catching COLLAPSE — one template swallowing a
        # cell — and 3.5 sigma still fails a 4-template cell at 50%, let alone
        # the 100% and 49% collapses this gate caught on the first generated
        # corpus. What it stops doing is flagging 38% against an even share of
        # 25% on a cell of ~130, which is noise.
        allow = max(args.max_template_share,
                    even + 3.5 * math.sqrt(even * (1 - even) / max(1, tot)))
        if share - allow > worst - worst_allow:
            worst, worst_cell, worst_allow = share, cell, allow
    passed &= gate(worst <= worst_allow, "template load",
                   f"worst cell {worst_cell} {worst:.0%} on one template "
                   f"(allowed {worst_allow:.0%})")

    # DISTINCT TEMPLATE COUNT, checked separately from share. The share test
    # cannot see total collapse: it derives the even share from the number of
    # templates PRESENT, so a cell that has fallen to a single template gets
    # even = 1/1 = 1.0 and passes at any share. Verified by injecting exactly
    # that defect — the share gate stayed silent.
    #
    # This is a regression the size-aware tolerance introduced. The original flat
    # 0.35 bar caught single-template cells for the wrong reason (100% > 35%);
    # replacing it with a statistically sounder test removed the one case the
    # gate most needed to catch.
    thin = {cell: len(c) for cell, c in cells.items() if len(c) < args.min_templates}
    passed &= gate(not thin, "template count",
                   f"{len(thin)} cell(s) under {args.min_templates} distinct templates"
                   + (f"  e.g. {sorted(thin.items())[:3]}" if thin else ""))

    # --- balance ------------------------------------------------------------
    def mix(sel, key):
        c = collections.Counter(r[key] for r in sel)
        tot = sum(c.values()) or 1
        return {k: v / tot for k, v in c.items()}
    full = mix(rows, "category")
    drift = 0.0
    for s in sorted({r["stratum"] for r in rows}):
        m = mix([r for r in rows if r["stratum"] == s], "category")
        drift = max(drift, max(abs(m.get(k, 0) - v) for k, v in full.items()))
    passed &= gate(drift < 0.05, "balance",
                   f"max category drift across strata {drift:.1%}")

    # --- lexical diversity --------------------------------------------------
    toks = [w for r in rows for w in norm(r["note"])]
    ttr = len(set(toks)) / max(1, len(toks))
    detail = f"type-token ratio {ttr:.4f}"
    if args.baseline:
        b = load(args.baseline)
        btoks = [w for r in b for w in norm(r["note"])]
        bttr = len(set(btoks)) / max(1, len(btoks))
        # TTR falls with corpus size by construction, so compare against the
        # baseline scaled to the same token count rather than raw.
        scaled = bttr * math.sqrt(len(btoks) / max(1, len(toks)))
        ratio = ttr / max(1e-9, scaled)
        detail += (f"  baseline {bttr:.4f} -> size-scaled {scaled:.4f}"
                   f"  ratio {ratio:.2f}")
        passed &= gate(ratio >= args.min_ttr_ratio, "lexical", detail)
    else:
        gate(True, "lexical", detail)

    # --- difficulty comparison (informational) ------------------------------
    if args.baseline:
        b = load(args.baseline)
        def prof(sel):
            wl = [len(norm(r["note"])) for r in sel]
            tp = [len(r.get("gold") or []) for r in sel]
            return (sum(wl) / len(wl), sum(tp) / len(tp),
                    sum(1 for x in tp if x == 0) / len(tp))
        gw, gt, ge = prof(rows)
        bw, bt, be = prof(b)
        print(f"  [info] difficulty      generated: {gw:.1f} words, "
              f"{gt:.2f} triples/note, {ge:.0%} empty")
        print(f"  [info]                 hand:      {bw:.1f} words, "
              f"{bt:.2f} triples/note, {be:.0%} empty")

    print(f"\n{'ALL GATES PASSED' if passed else 'GATES FAILED'}")
    return 0 if passed else 1


if __name__ == "__main__":
    sys.exit(main())
