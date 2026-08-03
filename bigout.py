import json

for arm in ("baseline", "aimee"):
    f = "/var/lib/aimee-workspaces/bench/raw/%s__am_e1af40a0f5__r1.jsonl" % arm
    outs = []
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
        outs.append((len(it.get("aggregated_output") or ""),
                     (it.get("command") or "").replace("\n", " ")[:120]))
    tot = sum(n for n, _ in outs)
    print("=== %s === commands=%d  total_output=%s chars (~%s tok)"
          % (arm, len(outs), format(tot, ","), format(tot // 4, ",")))
    for n, c in sorted(outs, reverse=True)[:6]:
        print("   {:>9,} chars  {}".format(n, c))
    print()
