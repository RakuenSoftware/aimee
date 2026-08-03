import json, sys

# Six span calls showed no file_path, which is what a batched call looks like to
# the sequence dump. Read the raw arguments to confirm `spans` is actually being
# used rather than inferring it from an absent field.
T = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"
f = "/var/lib/aimee-workspaces/bench/raw/aimee__%s__r1.jsonl" % T
batched = single = 0
for line in open(f, errors="replace"):
    try:
        d = json.loads(line)
    except Exception:
        continue
    if d.get("type") != "item.completed":
        continue
    it = d.get("item") or {}
    if (it.get("item_type") or it.get("type")) != "mcp_tool_call":
        continue
    args = it.get("arguments") or it.get("input") or {}
    if isinstance(args, str):
        try:
            args = json.loads(args)
        except Exception:
            args = {}
    name = (it.get("tool") or it.get("name") or "").split("__")[-1]
    if args.get("spans"):
        batched += 1
        n = len(args["spans"])
        print("BATCH span x%d raw: %s" % (n, json.dumps(args["spans"])[:300]))
    elif args.get("command") == "span":
        single += 1
        print("single span : %s:%s-%s" % (args.get("file_path"), args.get("line_start"),
                                          args.get("line_end")))
    if args.get("identifiers"):
        print("BATCH find_symbol x%d: %s" % (len(args["identifiers"]),
                                             ", ".join(map(str, args["identifiers"][:8]))))
    elif name == "find_symbol" and args.get("identifier"):
        print("single find_symbol: %s" % args.get("identifier"))
print("\nbatched span calls: %d   single span calls: %d" % (batched, single))
