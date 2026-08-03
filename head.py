import json
f="/var/lib/aimee-workspaces/bench/raw/aimee__am_e1af40a0f5__r1.jsonl"
n=0
for line in open(f, errors="replace"):
    try: d=json.loads(line)
    except Exception: continue
    it=d.get("item") or {}
    t=it.get("item_type") or it.get("type") or d.get("type")
    if t=="command_execution":
        n+=1
        if n<=3:
            print("--- CMD %d ---" % n)
            print((it.get("command") or "")[:900])
            print("   [out]", (it.get("aggregated_output") or "")[:500].replace("\n"," | "))
    if t=="agent_message" and n<=3:
        print("--- MSG ---"); print((it.get("text") or "")[:700])
    if n>3: break
