import json, re, sys, collections

TASK = sys.argv[1] if len(sys.argv) > 1 else "am_312e901904"
TOTALS = {  # measured input_tokens from summary.json, per arm, for this task
    "baseline": 3852870, "ponytail-instructions": 3162061,
    "ponytail-addon": 3212801, "aimee": 3734958,
}

def classify(b):
    ks = []
    if re.search(r'\brg --files|\bfind \.|\bls -R', b):        ks.append("survey")
    if re.search(r'\b(rg|grep)\b', b) and "--files" not in b:  ks.append("search")
    if re.search(r'\bsed -n|\bcat \b|\bhead -|\btail -', b):   ks.append("read")
    if re.search(r'\bmake\b|\bcmake\b|\bgcc\b|\bctest\b', b):  ks.append("build")
    if re.search(r'\bgit \b', b):                              ks.append("git")
    return ks or ["other"]

for arm in ("baseline", "aimee"):
    f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (arm, TASK)
    items = []          # (kind_list, out_tokens) in order
    mcp = 0
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        t = it.get("item_type") or it.get("type")
        if t == "command_execution":
            items.append((classify(re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)),
                          len(it.get("aggregated_output") or "") // 4))
        elif t == "mcp_tool_call":
            mcp += 1
            items.append((["mcp"], len(json.dumps(it)) // 4))

    n = len(items) + 1                       # one model call per result + final answer
    carried = collections.Counter()          # tokens this category forced onto later calls
    once = collections.Counter()
    for i, (ks, tok) in enumerate(items):
        resends = n - i - 1                  # how many later calls re-send this output
        for k in ks:
            carried[k] += (tok * resends) // len(ks)
            once[k] += tok // len(ks)

    explained = sum(carried.values())
    total = TOTALS[arm]
    base = (total - explained) / float(n)

    print("=== %s / %s ===" % (arm, TASK))
    print("  model calls: %d   total input tokens: %s" % (n, format(total, ",")))
    print("  %-8s %5s %10s %14s %7s" % ("kind", "n", "out_tok", "carried_tok", "% tot"))
    for k in sorted(carried, key=lambda x: -carried[x]):
        cnt = sum(1 for ks, _ in items if k in ks)
        print("  %-8s %5d %10s %14s %6.1f%%" % (k, cnt, format(once[k], ","),
              format(carried[k], ","), 100.0 * carried[k] / total))
    print("  %-8s %5s %10s %14s %6.1f%%" % ("BASE", n, "-", format(int(base * n), ","),
          100.0 * base * n / total))
    print("  (BASE = system prompt + tools/list + ticket, re-sent every call)")
    print()
