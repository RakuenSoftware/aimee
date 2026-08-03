import json

for arm, total_input in (("baseline", 441735), ("aimee", 4181646)):
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
        outs.append(len(it.get("aggregated_output") or "") // 4)  # ~tokens

    n = len(outs) + 1                      # one model call per result, plus the final answer
    prefix = 0
    accumulated = 0                        # sum over calls of "content already in context"
    for r in outs:
        accumulated += prefix
        prefix += r
    accumulated += prefix                  # final call sees everything

    base = (total_input - accumulated) / float(n)
    print("=== %s ===" % arm)
    print("  results: %d   total result tokens: %s" % (len(outs), format(sum(outs), ",")))
    print("  model calls (n): %d" % n)
    print("  tokens explained by accumulated results: %s" % format(accumulated, ","))
    print("  total input tokens: %s" % format(total_input, ","))
    print("  => fixed base per call: %s tok" % format(int(base), ","))
    print("  => base x n = %s tok (%.0f%% of total)"
          % (format(int(base * n), ","), 100.0 * base * n / total_input))
    print()
