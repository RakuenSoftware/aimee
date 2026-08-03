import json, glob, os, collections, sys

# What are the 30-40 index calls per cell actually asking for? If they are paging
# narrow result sets, max_results is the lever. If they are distinct questions,
# the turn count is inherent and tuning will not help.
TASK = sys.argv[1] if len(sys.argv) > 1 else None
pat = "/var/lib/aimee-workspaces/bench/raw/aimee__%s__r1.jsonl" % TASK if TASK \
      else "/var/lib/aimee-workspaces/bench/raw/aimee__*__r1.jsonl"

cmds = collections.Counter(); queries = []; sizes = []
for f in sorted(glob.glob(pat)):
    if os.path.getmtime(f) < 1785780000:   # only the current (verified-stack) run
        continue
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "mcp_tool_call": continue
        name = it.get("tool") or it.get("name") or ""
        blob = json.dumps(it)
        sizes.append(len(blob))
        if "index" not in name: continue
        args = it.get("arguments") or it.get("input") or {}
        if isinstance(args, str):
            try: args = json.loads(args)
            except Exception: args = {}
        cmds[args.get("command", "?")] += 1
        q = args.get("query") or args.get("symbol") or args.get("file_path") or ""
        if q: queries.append("%s: %s" % (args.get("command", "?"), str(q)[:64]))

print("index commands used:")
for k, v in cmds.most_common(): print("   %-16s %d" % (k, v))
print("\nmcp result sizes: n=%d  mean=%d chars  max=%d" %
      (len(sizes), sum(sizes)//len(sizes) if sizes else 0, max(sizes) if sizes else 0))
print("\nfirst 18 queries:")
for q in queries[:18]: print("   ", q)
dupes = len(queries) - len(set(queries))
print("\nrepeated identical queries: %d of %d" % (dupes, len(queries)))
