import json, re, glob, os, collections

buckets = [(0, 2_000), (2_000, 10_000), (10_000, 50_000), (50_000, 200_000), (200_000, 10**9)]
names = ["<2k", "2-10k", "10-50k", "50-200k", ">200k"]
dist = collections.defaultdict(lambda: [0] * len(buckets))
tot = collections.Counter(); cnt = collections.Counter()
wide = collections.Counter(); capped = collections.Counter()
scoped = collections.Counter()

for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/*__r1.jsonl")):
    arm = os.path.basename(f).split("__")[0]
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "command_execution": continue
        c = re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)
        out = len(it.get("aggregated_output") or "")
        frags = [x.strip() for x in re.split(r'&&|\|\||;', c)]
        srch = [x for x in frags if re.match(r'^["\']?\s*(rg|grep)\b', x) and "--files" not in x]
        if not srch: continue
        cnt[arm] += len(srch); tot[arm] += out
        for i, (lo, hi) in enumerate(buckets):
            if lo <= out < hi: dist[arm][i] += 1; break
        for s in srch:
            # a "wide" query: 3+ alternations in the pattern
            if s.count("|") >= 3: wide[arm] += 1
            if "head" in c: capped[arm] += 1
            # scoped: names a path other than the repo root
            if re.search(r'\bsrc/\S|\btests?/\S|\b\S+\.(c|h|py|md)\b', s): scoped[arm] += 1

print("%-22s %6s %12s %8s %8s %8s   %s" % ("arm", "srch", "out_chars", "wide", "capped", "scoped", "size distribution " + str(names)))
for arm in sorted(cnt):
    print("%-22s %6d %12s %8d %8d %8d   %s" % (arm, cnt[arm], format(tot[arm], ","),
          wide[arm], capped[arm], scoped[arm], dist[arm]))
