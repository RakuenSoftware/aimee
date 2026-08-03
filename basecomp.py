import json, sys
# The isolation run: 1 command, 2 model calls. Decompose what is IN that context.
f = "/tmp/base-isolate/%s.jsonl" % sys.argv[1]
n_items = 0
for line in open(f, errors="replace"):
    try: d = json.loads(line)
    except Exception: continue
    t = d.get("type")
    it = d.get("item") or {}
    if t == "item.completed":
        n_items += 1
        kind = it.get("item_type") or it.get("type")
        blob = json.dumps(it)
        print("  item %-18s %6d chars (~%d tok)" % (kind, len(blob), len(blob)//4))
    u = d.get("usage") or {}
    if u:
        print("  USAGE input=%s cached=%s output=%s" % (
            format(u.get("input_tokens",0), ","), format(u.get("cached_input_tokens",0), ","),
            format(u.get("output_tokens",0), ",")))
