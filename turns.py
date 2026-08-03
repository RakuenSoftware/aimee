import json, sys, collections
RAW = "/var/lib/aimee-workspaces/bench/raw/%s__am_e1af40a0f5__r1.jsonl"
for arm in ("baseline", "aimee"):
    print("=== %s ===" % arm)
    tot = collections.Counter(); cmds = []
    prev_in = 0; turns = []
    for line in open(RAW % arm, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        t = d.get("type") or ""
        item = d.get("item") or {}
        it = item.get("item_type") or item.get("type") or ""
        tot[t + "/" + it] += 1
        if it == "command_execution":
            c = (item.get("command") or "")[:110].replace("\n", " ")
            cmds.append(c)
        u = (d.get("usage") or {})
        if u:
            turns.append(u.get("input_tokens", 0))
    print("  events:", sum(tot.values()))
    print("  usage samples:", len(turns), "input_tokens seq:", turns[:12])
    print("  first 22 commands:")
    for c in cmds[:22]:
        print("   ", c)
    print("  total commands:", len(cmds))
