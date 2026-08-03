import json, glob, os, collections
rows = collections.defaultdict(dict)
for f in sorted(glob.glob("/opt/bench/results/cells/*/summary.json")):
    d = os.path.basename(os.path.dirname(f))
    arm, task, _ = d.split("__")
    s = json.load(open(f))
    cx = s.get("codex", {}) or {}
    try:
        p = json.load(open(f.replace("summary", "hidden")))["passed"]
    except Exception:
        p = None
    tc = cx.get("tool_calls", {}) or {}
    mcp = sum(v for k, v in tc.items() if k.startswith("mcp:"))
    rows[task][arm] = (p, round(cx.get("estimated_credits", 0) or 0, 1), mcp,
                       sum(tc.values()))
for task in sorted(rows):
    print(task)
    for arm in ("baseline", "ponytail-instructions", "ponytail-addon", "aimee"):
        if arm in rows[task]:
            p, c, mcp, tot = rows[task][arm]
            print("   %-22s %-4s %6.1f cr   %3d tools (%d mcp)" %
                  (arm, "PASS" if p else "FAIL", c, tot, mcp))
        else:
            print("   %-22s   --" % arm)
