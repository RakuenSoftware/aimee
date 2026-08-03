import json, glob, os, re, collections

# The harness's own rate card (codex_matrix_runner.py, provenance
# credit_rate_per_million). Cached input is already discounted 10:1 here, so
# credits ARE the cost-weighted number -- a raw input-token ratio overstates the
# gap badly, because a re-sent prefix is almost entirely cache hits.
UNCACHED, CACHED, OUTPUT = 125.0, 12.5, 750.0
# Published gpt-5.6-sol rates, effective 2026-07-30: $5.00/MTok input,
# $30.00/MTok output, cached input at 10% of input ($0.50/MTok).
#
# These are the card the harness itself was built on -- its credit rates divide
# through to a single constant: 5.00/125 = 0.50/12.5 = 30.00/750 = 0.04. So one
# credit is exactly $0.04 and USD is a clean rescaling of credits, not a
# reweighting. (Do NOT use benchmarks/coding/cost_savings.py DEFAULT_PRICE here:
# that card is self-described as an assumption for pricing FREE LOCAL models at
# frontier-equivalent rates, it predates 5.6, and it has no cached tier at all --
# which is disqualifying when 92-96% of these tokens are cache hits.)
USD_IN, USD_CACHED, USD_OUT = 5.00, 0.50, 30.00
ARMS = ["baseline", "ponytail-instructions", "ponytail-addon", "aimee"]
RAW = "/var/lib/aimee-workspaces/bench/raw"
CUTOFF = 1785780000  # only the current verified-stack run


def cell(arm, task):
    f = "%s/%s__%s__r1.jsonl" % (RAW, arm, task)
    if not os.path.exists(f):
        return None
    if arm == "aimee" and os.path.getmtime(f) < CUTOFF:
        return None
    tin = tcache = tout = 0
    calls = collections.Counter()
    for line in open(f, errors="replace"):
        try:
            d = json.loads(line)
        except Exception:
            continue
        u = d.get("usage") or (d.get("item") or {}).get("usage")
        if isinstance(u, dict):
            tin += u.get("input_tokens", 0) or 0
            tcache += u.get("cached_input_tokens", 0) or 0
            tout += u.get("output_tokens", 0) or 0
        if d.get("type") == "item.completed":
            ty = (d.get("item") or {}).get("item_type") or (d.get("item") or {}).get("type")
            if ty in ("command_execution", "local_shell_call"):
                calls["shell"] += 1
            elif ty == "mcp_tool_call":
                calls["mcp"] += 1
    unc = max(0, tin - tcache)
    return {
        "unc_cr": unc * UNCACHED / 1e6, "cac_cr": tcache * CACHED / 1e6,
        "out_cr": tout * OUTPUT / 1e6,
        "cr": (unc * UNCACHED + tcache * CACHED + tout * OUTPUT) / 1e6,
        "usd": (unc * USD_IN + tcache * USD_CACHED + tout * USD_OUT) / 1e6,
        "in": tin, "cached": tcache, "out": tout, "unc": unc,
        "calls": calls["shell"] + calls["mcp"],
    }


tasks = sorted({re.sub(r".*__(am_[0-9a-f]+)__.*", r"\1", os.path.basename(p))
                for p in glob.glob("%s/*__am_*__r1.jsonl" % RAW)
                if os.path.getmtime(p) >= CUTOFF})

H = "%-14s %-22s %10s %10s %9s %9s %9s %9s %8s %6s"
print(H % ("task", "arm", "in_tok", "cached", "out_tok", "credits", "USD", "vs base", "cachehit", "calls"))
tot = collections.defaultdict(lambda: collections.Counter())
for t in tasks:
    _b = cell("baseline", t)
    per_task_base = {t: _b["usd"]} if _b else {}
    for a in ARMS:
        r = cell(a, t)
        if not r:
            continue
        hit = 100.0 * r["cached"] / r["in"] if r["in"] else 0
        bc = per_task_base.get(t)
        rel = ("%.2fx" % (r["usd"] / bc)) if bc else "-"
        print(H % (t, a, format(r["in"], ","), format(r["cached"], ","),
                   format(r["out"], ","), "%.1f" % r["cr"],
                   "$%.4f" % r["usd"], rel, "%.1f%%" % hit, r["calls"]))
        for k in ("unc_cr", "cac_cr", "out_cr", "cr", "usd", "calls"):
            tot[a][k] += r[k]
        tot[a]["n"] += 1
    print()

print("=" * 92)
print("%-14s %-22s %10s %10s %9s %9s %9s %9s %8s %6s" %
      ("TOTAL", "arm", "unc_cr", "cac_cr", "out_cr", "credits", "USD", "vs base", "", "calls"))
base = tot["baseline"]["usd"] or 1
for a in ARMS:
    if not tot[a]["n"]:
        continue
    print("%-14s %-22s %10.1f %10.1f %9.1f %9.1f %9s %9s %8s %6d   (n=%d)" %
          ("", a, tot[a]["unc_cr"], tot[a]["cac_cr"], tot[a]["out_cr"], tot[a]["cr"],
           "$%.4f" % tot[a]["usd"], "%.2fx" % (tot[a]["usd"] / base),
           "", tot[a]["calls"], tot[a]["n"]))
