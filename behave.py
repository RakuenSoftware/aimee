import json, re, collections, sys

TASK = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"

def classify(cmd):
    """What kind of work is this command doing?"""
    b = re.sub(r'^/bin/bash\s+-lc\s+', '', cmd, flags=re.S)
    kinds = []
    if re.search(r'\brg --files|\bfind \.|\bls -R', b):      kinds.append("survey")
    if re.search(r'\b(rg|grep)\b', b) and "rg --files" not in b: kinds.append("search")
    if re.search(r'\bsed -n|\bcat \b|\bhead -|\btail -', b): kinds.append("read")
    if re.search(r'\bmake\b|\bcmake\b|\bgcc\b', b):          kinds.append("build")
    if re.search(r'\bgit \b', b):                            kinds.append("git")
    if re.search(r'apply_patch|>\s*\S+\.(c|h|py)', b):       kinds.append("edit")
    return kinds or ["other"]

for arm in ("baseline", "aimee"):
    f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (arm, TASK)
    try:
        lines = open(f, errors="replace").read().splitlines()
    except FileNotFoundError:
        print("=== %s: no transcript ===" % arm); continue
    kind_n = collections.Counter(); kind_out = collections.Counter()
    seq = []
    for line in lines:
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        t = it.get("item_type") or it.get("type")
        if t == "command_execution":
            c = it.get("command") or ""
            out = len(it.get("aggregated_output") or "")
            ks = classify(c)
            for k in ks:
                kind_n[k] += 1; kind_out[k] += out // len(ks)
            seq.append("+".join(ks))
        elif t == "mcp_tool_call":
            seq.append("MCP:" + (it.get("tool") or it.get("name") or "?"))
    print("=== %s (%s) ===" % (arm, TASK))
    print("  %-9s %5s %12s" % ("kind", "n", "out_chars"))
    for k in sorted(kind_n, key=lambda x: -kind_out[x]):
        print("  %-9s %5d %12s" % (k, kind_n[k], format(kind_out[k], ",")))
    print("  sequence:", " ".join(seq[:26]))
    print()
