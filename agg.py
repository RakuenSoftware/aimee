import json, re, glob, os, collections

def classify(b):
    ks = []
    if re.search(r'\brg --files|\bfind \.|\bls -R', b):            ks.append("survey")
    if re.search(r'\b(rg|grep)\b', b) and "--files" not in b:      ks.append("search")
    if re.search(r'\bsed -n|\bcat \b|\bhead -|\btail -', b):       ks.append("read")
    if re.search(r'\bmake\b|\bcmake\b|\bgcc\b|\bctest\b', b):      ks.append("build")
    if re.search(r'\bgit \b', b):                                  ks.append("git")
    return ks or ["other"]

agg = collections.defaultdict(lambda: collections.defaultdict(lambda: [0, 0]))
uncapped = collections.Counter()
searches = collections.Counter()
for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/*__r1.jsonl")):
    cell = os.path.basename(f)[:-6]
    arm = cell.split("__")[0]
    if arm not in ("baseline", "aimee"): continue
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "command_execution": continue
        c = re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)
        out = len(it.get("aggregated_output") or "")
        ks = classify(c)
        for k in ks:
            agg[arm][k][0] += 1
            agg[arm][k][1] += out // len(ks)
        for frag in re.split(r'&&|\|\||;', c):
            frag = frag.strip()
            if re.match(r'^["\']?\s*(rg|grep)\b', frag) and "--files" not in frag:
                searches[arm] += 1
                if "head" not in frag:
                    uncapped[arm] += 1

print("%-10s %-8s %6s %14s" % ("arm", "kind", "n", "out_chars"))
for arm in ("baseline", "aimee"):
    for k in sorted(agg[arm], key=lambda x: -agg[arm][x][1]):
        n, o = agg[arm][k]
        print("%-10s %-8s %6d %14s" % (arm, k, n, format(o, ",")))
    print()
print("search fragments / of which UNCAPPED (no head):")
for arm in ("baseline", "aimee"):
    print("  %-10s %d / %d" % (arm, searches[arm], uncapped[arm]))
