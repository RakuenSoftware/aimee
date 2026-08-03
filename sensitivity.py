import json, glob, os, re, collections

# We do not have an authoritative gpt-5.6-sol rate card. The repo's existing card
# is self-described as an assumption for pricing FREE LOCAL models at
# frontier-equivalent rates, and carries no cached tier at all -- yet 92-96% of
# every arm's tokens here are cache hits, so the cached rate dominates the answer.
#
# Rather than publish one guessed number, sweep the two parameters we do not know
# and report the range. Absolute dollars scale with the input price, so hold that
# at 1.0 and vary what actually reorders the arms: the cached discount and the
# output multiple. If aimee's ratio stays on one side of 1.0 across the whole
# sweep, the verdict is robust to the card and only the dollar label is uncertain.
RAW = "/var/lib/aimee-workspaces/bench/raw"
CUTOFF = 1785780000
ARMS = ["baseline", "ponytail-instructions", "ponytail-addon", "aimee"]


def usage(arm, t):
    f = "%s/%s__%s__r1.jsonl" % (RAW, arm, t)
    if not os.path.exists(f):
        return None
    if arm == "aimee" and os.path.getmtime(f) < CUTOFF:
        return None
    tin = tc = to = 0
    for line in open(f, errors="replace"):
        try:
            d = json.loads(line)
        except Exception:
            continue
        u = d.get("usage") or (d.get("item") or {}).get("usage")
        if isinstance(u, dict):
            tin += u.get("input_tokens", 0) or 0
            tc += u.get("cached_input_tokens", 0) or 0
            to += u.get("output_tokens", 0) or 0
    if tin == 0:
        return None  # broken cell: the run recorded no usage events
    return tin - tc, tc, to


def totals(arm, tasks):
    unc = cac = out = 0
    for t in tasks:
        u, c, o = usage(arm, t)
        unc += u
        cac += c
        out += o
    return unc, cac, out


tasks = sorted({re.sub(r".*__(am_[0-9a-f]+)__.*", r"\1", os.path.basename(p))
                for p in glob.glob("%s/aimee__am_*__r1.jsonl" % RAW)
                if os.path.getmtime(p) >= CUTOFF})
# A task counts only if EVERY arm produced a usable cell for it. Dropping a
# broken cell from one arm alone silently compares different task sets -- which
# is exactly the error that made an earlier CT401 run read 0.62x.
dropped = [t for t in tasks if any(usage(a, t) is None for a in ARMS)]
tasks = [t for t in tasks if t not in dropped]
if dropped:
    print("DROPPED (no usable cell in some arm): %s" % ", ".join(dropped))
data = {a: totals(a, tasks) for a in ARMS}
if "baseline" not in data:
    raise SystemExit("no baseline")

print("tasks on this box: %s" % ", ".join(tasks))
print("%-22s %12s %12s %12s" % ("arm", "uncached", "cached", "output"))
for a in ARMS:
    if a in data:
        print("%-22s %12s %12s %12s" % ((a,) + tuple(format(x, ",") for x in data[a])))
print()

print("aimee cost relative to baseline, sweeping the two unknown parameters")
print("(input price held at 1.0 -- it scales dollars but cancels in the ratio)")
print()
print("%-14s %s" % ("", "  ".join("out=%-5.1fx" % o for o in (4, 6, 8, 10, 15))))
for cd in (0.05, 0.10, 0.125, 0.25, 0.50, 1.00):
    row = []
    for om in (4, 6, 8, 10, 15):
        def cost(a):
            u, c, o = data[a]
            return u * 1.0 + c * cd + o * om
        row.append("%.2fx" % (cost("aimee") / cost("baseline")))
    print("cached=%-7s %s" % ("%.3f" % cd, "  ".join("%-10s" % r for r in row)))
