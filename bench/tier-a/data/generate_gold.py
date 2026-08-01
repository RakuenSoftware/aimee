"""Generate the tiered gold sets. Step 4 of HARNESS_DESIGN.md.

Seeded and deterministic: the same seed and inventories reproduce the corpus
byte for byte, so a published number can be traced to the exact set it came from.

TWO WAYS OF SLICING, EMITTED TOGETHER, BECAUSE THEY ANSWER DIFFERENT QUESTIONS.

  tier     1/2/3, NESTED. small = tier 1; mid = tiers 1-2; large = all three.
           Nesting buys comparability: a cheap small-tier run is directly
           comparable to an expensive large-tier one because small's notes are a
           literal subset. It does NOT buy replication — a result holding on
           small and again on large is the same data with more of it, not two
           confirmations, and anything published must say so.

  stratum  S1..Sk, DISJOINT. This is where replication comes from. Agreement in
           direction across disjoint strata is a sign test: under the null each
           is positive with probability 0.5, so 11 of 11 is p = 4.9e-4. That is
           how an effect too small for any single sample to resolve gets
           established — provided the samples are genuinely independent.

           Re-running the SAME stratum is not a second observation. This harness
           is deterministic: two runs of one model at one setting produced
           byte-identical output on all 70 notes of the original set. Six runs of
           one corpus is one data point, not six.

Assignment is stratified: tiers and strata are filled proportionally across every
(domain, category) cell, so each slice holds the same mix. Taking a prefix of a
generated file would give small whatever the loop happened to emit first.

FACTS ARE CONSUMED, NOT SAMPLED WITH REPLACEMENT. A fact used twice would put
near-duplicate notes in different strata and break their independence, which is
the one property the sign test depends on.
"""
import argparse
import collections
import json
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import templates as T  # noqa: E402

# Proportions of the original 70-note set, which were chosen for the failure
# modes they probe rather than for balance. transient is the largest slice
# because over-extraction is the expensive failure.
MIX = {"transient": 0.17, "first_person": 0.11, "multi_fact": 0.11,
       "third_person": 0.10, "implicit": 0.10, "negation": 0.09,
       "novel_pred": 0.09, "infra": 0.09, "ambiguous": 0.07,
       "governance": 0.07}

DOMAIN_SHARE = {"code": 0.40, "business": 0.30, "sales": 0.30}

# aimee is code-heavy, so the code share is held at 40% rather than allowed to
# sag to whatever the RakuenSoftware and JBailes repos happen to supply (~25%).
# torvalds/linux fills the gap: renames, moves, deletions and authorship are
# structurally identical facts regardless of what the code does, so
# `mm/slub.c -> mm/slab_common.c` exercises the same extraction as
# `learning.h -> db2_learning.h`.
#
# It is CAPPED rather than merged wholesale. Linux carries ~1.4M commits against
# ~18k across every other repo, so an uncapped merge would make "the code domain"
# mean "the kernel" and collapse entity diversity onto subsystem names. The cap
# is expressed as a share of code-domain facts, so the kernel supplements the
# owned repos instead of replacing them.
LINUX_MAX_SHARE = float(os.environ.get("LINUX_MAX_SHARE", "0.40"))


def code_facts(inv, rng):
    """Flatten the mined inventory into per-fact-kind pools."""
    pools = {k: [] for k in
             ("rename", "move", "deletion", "authorship", "authorship2",
              "version", "repo", "host")}
    repos = sorted(inv["repos"])
    for repo, v in inv["repos"].items():
        for r in v["renames"]:
            base_old, base_new = os.path.basename(r["old"]), os.path.basename(r["new"])
            if r["kind"] == "rename":
                pools["rename"].append({"repo": repo, "old_base": base_old,
                                        "new_base": base_new, "sha": r["sha"]})
            else:
                od = os.path.dirname(r["old"]) or "the repo root"
                nd = os.path.dirname(r["new"]) or "the repo root"
                if od != nd:
                    pools["move"].append({"repo": repo, "base": base_new,
                                          "old_dir": od, "new_dir": nd,
                                          "sha": r["sha"]})
        for d in v["deletions"]:
            pools["deletion"].append({"repo": repo,
                                      "base": os.path.basename(d["path"]),
                                      "sha": d["sha"]})
        vs = v["versions"]
        for a, b in zip(vs, vs[1:]):
            pools["version"].append({"repo": repo, "old_ver": a, "new_ver": b})
        # A single drive-by commit does not make someone a contributor in the
        # sense a person would assert in a remembered note.
        for person, n in v["authors"].items():
            if n >= 2:
                pools["authorship"].append({"repo": repo, "person": person})
        # Transient notes are ABOUT a repo rather than derived from a fact, so
        # this pool is bounded by the generator rather than by the data.
        for _ in range(12):
            pools["repo"].append({"repo": repo})

    for person, repo_map in inv["people"].items():
        strong = [r for r, n in repo_map.items() if n >= 3]
        if len(strong) >= 2:
            pools["authorship2"].append({"person": person, "repo": strong[0],
                                         "repo2": strong[1]})

    # infra: service names are real, addresses are synthetic. A real internal IP
    # would be an operational detail leaking into a public benchmark.
    for i, repo in enumerate(repos):
      for env in ("prod", "stage", "build", "dev", "ci", "edge", "qa",
                  "canary", "sandbox", "perf", "backup", "replica"):
        pools["host"].append({
            "service": repo, "host": f"{repo.lower()}-{env}-{i%9+1}",
            "ip": f"10.{rng.randint(20,60)}.{rng.randint(0,255)}.{rng.randint(2,254)}",
            "repo": repo})
    # Cap any single repo's contribution to each pool. Without this the kernel,
    # at ~1.4M commits against ~18k everywhere else, would supply essentially
    # every rename and deletion and the code domain would stop being about the
    # owned repos at all.
    for kind, pool in pools.items():
        by_repo = {}
        for f in pool:
            by_repo.setdefault(f.get("repo", "?"), []).append(f)
        if len(by_repo) > 1:
            # Cap the dominant repo at LINUX_MAX_SHARE of the FINAL pool, not of
            # the raw pool. Capping at a fraction of the raw total let the kernel
            # keep 40% of a pool it already supplied 80% of, so it still ended up
            # at 50% of code notes. Solving share = cap/(cap+others) for cap
            # gives cap = others * share/(1-share).
            biggest = max(len(v) for v in by_repo.values())
            others = len(pool) - biggest
            cap = max(1, int(others * LINUX_MAX_SHARE / (1 - LINUX_MAX_SHARE)))
            trimmed = []
            for repo, items in by_repo.items():
                rng.shuffle(items)
                trimmed.extend(items[:cap] if len(items) > cap else items)
            pools[kind] = trimmed
    for p in pools.values():
        rng.shuffle(p)
    return pools


def synth_fields(kind, syn, rng):
    """One row of business/sales fields, internally consistent by construction."""
    person = rng.choice(syn["people"])
    company = rng.choice(syn["companies"])
    contract = rng.choice(syn["contracts"])
    product = rng.choice(syn["products"])
    year = rng.randint(2024, 2027)
    return {
        "person": person["name"], "role": person["role"], "team": person["team"],
        # Employer comes from the person record, never re-rolled, or a note and
        # its neighbour could assert two different employers for one person.
        "company": company["name"] if kind == "company" else person["employer"],
        "city": company["city"] if kind == "company" else person["employer_city"],
        "tier": company["tier"], "product": product["name"],
        "contract": contract["name"], "renews_on": contract["renews_on"],
        "policy": rng.choice(["retention policy", "pricing sheet",
                              "discount schedule", "escalation policy",
                              "data processing agreement"]),
        "year": str(year), "prev_year": str(year - 1),
    }


def render(tpl, fields):
    """Render a template into (note, gold).

    A template may carry `alt`: per-gold-triple lists of ALTERNATIVE RELATION
    NAMES that express the same fact. score.py expects full alternative triples,
    so they are expanded here against the same subject and object.

    This exists for minted predicates, which have no canonical spelling. Gold
    that demands exactly `renews_on` where a model says `renewal_date` measures
    spelling luck, not extraction — and score.py counts an alternative as a true
    positive rather than excusing it from the denominator.
    """
    note = tpl["text"].format(**fields)
    gold = []
    alts = tpl.get("alt") or []
    for i, g in enumerate(tpl["gold"]):
        t = {"subject": g["s"].format(**fields),
             "relation": g["r"].format(**fields),
             "object": g["o"].format(**fields)}
        if i < len(alts):
            t["alt"] = [{"subject": t["subject"], "relation": r,
                         "object": t["object"]} for r in alts[i]]
        gold.append(t)
    return note, gold


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--inventory", required=True)
    ap.add_argument("--synth", required=True)
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--large", type=int, default=10000)
    ap.add_argument("--mid", type=int, default=3000)
    ap.add_argument("--small", type=int, default=1000)
    ap.add_argument("--strata", type=int, default=10)
    # 20260802, not 20260801. The original seed produced a corpus whose LARGE
    # tier passed every gate while its SMALL tier failed template load: one
    # sales/negation template drew 23 of 40 tier-1 slots from a cell that is
    # 135/135/134 overall. Tiers 2 and 3 of the same cell were balanced, so the
    # stratification is sound and that draw was simply unlucky — two other seeds
    # pass cleanly. HARNESS_DESIGN.md says a tier that fails a gate is
    # regenerated rather than shipped with a footnote, so it was. Recorded here
    # because silently swapping a seed after seeing results is otherwise
    # indistinguishable from tuning until the numbers look good.
    ap.add_argument("--seed", type=int, default=20260802)
    args = ap.parse_args()

    rng = random.Random(args.seed)
    inv = json.load(open(args.inventory))
    syn = json.load(open(args.synth))
    pools = code_facts(inv, rng)

    rows, exhausted, seen_notes = [], [], set()
    tpl_used = collections.Counter()
    def emit(domain, dshare, budget):
        bank = T.BANK[domain]
        # Categories with no templates in this domain get their share spread over
        # the ones that exist, rather than silently shrinking the domain.
        live = {c: w for c, w in MIX.items() if c in bank}
        norm = sum(live.values())
        for cat, w in live.items():
            want = int(round(budget * dshare * w / norm))
            tpls = bank[cat]
            for i in range(want):
                # Rotate to the NEXT template when a render collides, rather
                # than dropping the slot. Dropping skewed template share badly:
                # low-slot templates exhaust their entity variety first, get
                # rejected most, and one high-variety template ends up carrying
                # 42% of its cell — exactly the collapse the load gate exists to
                # catch.
                # LEAST-USED FIRST, not round-robin. Templates differ enormously
                # in how many distinct notes they can render: "I sit on the
                # {team}." exhausts after ~10, while one naming a person and a
                # company has hundreds of thousands. Round-robin plus
                # collision-rotation therefore drained the narrow templates early
                # and let the widest one absorb ~49% of its cell. Choosing the
                # currently least-used template that can still produce a fresh
                # note balances the cell directly instead of hoping.
                emitted = False
                order = sorted(range(len(tpls)),
                               key=lambda j: (tpl_used[(domain, cat, j)], j))
                for ti in order:
                    tpl = tpls[ti]
                    if domain == "code":
                        kind = tpl["fact"]
                        if not pools[kind]:
                            exhausted.append(f"{domain}/{cat}/{kind}")
                            continue
                        fields = pools[kind].pop()
                        note, gold = render(tpl, fields)
                        if note in seen_notes:
                            continue
                    else:
                        fields = synth_fields("company", syn, rng)
                        note, gold = render(tpl, fields)
                        tries = 0
                        while note in seen_notes and tries < 40:
                            fields = synth_fields("company", syn, rng)
                            note, gold = render(tpl, fields)
                            tries += 1
                        if note in seen_notes:
                            continue
                    seen_notes.add(note)
                    tpl_used[(domain, cat, ti)] += 1
                    rows.append({
                        "domain": domain, "category": cat, "note": note,
                        "gold": gold,
                        "template": f"{domain}.{cat}.{ti}",
                        "source": {k: v for k, v in fields.items()
                                   if k in ("repo", "sha")} or None})
                    emitted = True
                    break
                if not emitted:
                    break

    for _domain, _dshare in DOMAIN_SHARE.items():
        emit(_domain, _dshare, args.large)
    # The code domain is capped by how many facts git can prove. Rather than
    # reuse a fact — which would put near-duplicates in different strata and
    # break the independence the sign test needs — the shortfall goes to the
    # synthetic domains, and the ACTUAL split is reported below.
    # Top up CODE first. The code domain is the one with a hard supply limit, so
    # any shortfall used to be handed straight to the synthetic domains and code
    # settled ~13 points under its 40% target. Give it a second pass against the
    # pools that still have facts before letting business and sales absorb the
    # remainder.
    for _ in range(3):
        short = args.large - len(rows)
        if short <= 0:
            break
        before = len(rows)
        emit("code", 1.0, short)
        if len(rows) == before:
            break
    short = args.large - len(rows)
    if short > 0:
        for _domain in ("business", "sales"):
            emit(_domain, 0.5, short)

    # Stratified assignment. Shuffle within each cell, then deal round-robin so
    # every tier and stratum gets the same mix.
    by_cell = {}
    for r in rows:
        by_cell.setdefault((r["domain"], r["category"]), []).append(r)
    ordered = []
    for cell in sorted(by_cell):
        rng.shuffle(by_cell[cell])
        ordered.append(by_cell[cell])

    # Assign tier and stratum WITHIN each cell, so both slices carry the same
    # (domain, category) mix by construction. Dealing round-robin and then
    # striping by global index looked stratified and was not: with ~30 cells and
    # 10 strata, cell i landed in stratum (i mod 10) every time, giving each
    # stratum a fixed subset of cells and 20% category drift.
    total = sum(len(c) for c in ordered)
    small_frac = args.small / max(1, total)
    mid_frac = args.mid / max(1, total)
    dealt = []
    for cell in ordered:
        k = len(cell)
        for j, r in enumerate(cell):
            r["tier"] = (1 if j < round(k * small_frac)
                         else 2 if j < round(k * mid_frac) else 3)
            r["stratum"] = f"S{j % args.strata + 1}"
            r["provenance"] = "generated"
            dealt.append(r)
    rng.shuffle(dealt)
    for n, r in enumerate(dealt):
        r["id"] = f"g{n:06d}"

    os.makedirs(args.out_dir, exist_ok=True)
    tiers = {"small": [r for r in dealt if r["tier"] == 1],
             "mid": [r for r in dealt if r["tier"] <= 2],
             "large": dealt}
    for name, sel in tiers.items():
        path = os.path.join(args.out_dir, f"gold_{name}.jsonl")
        with open(path, "w") as fh:
            for r in sel:
                fh.write(json.dumps(r, ensure_ascii=False) + "\n")
        empty = sum(1 for r in sel if not r["gold"])
        trip = sum(len(r["gold"]) for r in sel)
        print(f"{name:6} {len(sel):6} notes  {trip:6} triples  "
              f"{empty:5} empty-gold ({100*empty/max(1,len(sel)):.0f}%)")
    if exhausted:
        print("\nfact pools exhausted (fewer notes than requested):")
        for e in sorted(set(exhausted)):
            print(f"  {e}")


if __name__ == "__main__":
    main()
