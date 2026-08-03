import json, glob, os, re, collections

stat = collections.Counter()
examples = []
for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/aimee__*__r1.jsonl")):
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
        c = it.get("command") or ""
        # strip the /bin/bash -lc wrapper
        m = re.match(r'^/bin/bash\s+-lc\s+(.*)$', c, re.S)
        body = m.group(1) if m else c
        compound = bool(re.search(r'&&|\|\||;|\|', body))
        has_search = bool(re.search(r'\b(rg|grep|ag)\b', body))
        stat["total"] += 1
        stat["compound" if compound else "simple"] += 1
        if has_search:
            stat["has_search"] += 1
            stat["search_compound" if compound else "search_simple"] += 1
            if not compound and len(examples) < 5:
                examples.append(body[:150])

print("aimee-arm shell commands across all cells:")
for k in ("total", "simple", "compound", "has_search", "search_simple", "search_compound"):
    print("  %-16s %d" % (k, stat[k]))
print("\nsimple search commands a rewrite could target:")
for e in examples:
    print("   ", e)
