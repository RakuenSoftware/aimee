import json, glob, os

rows = []
for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/*__r1.jsonl")):
    cell = os.path.basename(f)[:-6]
    arm, task, _ = cell.split("__")
    outs, order = [], []
    for line in open(f, errors="replace"):
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") != "item.completed":
            continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "command_execution":
            continue
        n = len(it.get("aggregated_output") or "")
        outs.append(n)
        order.append(n)
    if not outs:
        continue
    # tokens that ride in context for the REST of the run = sum(size * calls_remaining)
    carry = sum(sz * (len(order) - i - 1) for i, sz in enumerate(order)) // 4
    rows.append((task, arm, len(outs), sum(outs) // 4, max(outs) // 4, carry))

rows.sort()
print("%-16s %-22s %5s %9s %9s %12s" % ("task", "arm", "cmds", "out_tok", "max_out", "carry_tok"))
for t, a, n, s, m, c in rows:
    print("%-16s %-22s %5d %9s %9s %12s" % (t, a, n, format(s, ","), format(m, ","), format(c, ",")))
