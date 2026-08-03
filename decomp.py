import json, collections
for arm in ("baseline","aimee"):
    f="/var/lib/aimee-workspaces/bench/raw/%s__am_e1af40a0f5__r1.jsonl"%arm
    turns=0; outbytes=0; cmdbytes=0; ncmd=0; types=collections.Counter()
    usages=[]
    for line in open(f, errors="replace"):
        try: d=json.loads(line)
        except Exception: continue
        t=d.get("type",""); types[t]+=1
        if t=="turn.completed":
            turns+=1
            u=d.get("usage") or {}
            usages.append((u.get("input_tokens"),u.get("cached_input_tokens"),u.get("output_tokens")))
        it=d.get("item") or {}
        if (it.get("item_type") or it.get("type"))=="command_execution" and t=="item.completed":
            ncmd+=1
            outbytes+=len(it.get("aggregated_output") or "")
            cmdbytes+=len(it.get("command") or "")
    print("=== %s ===" % arm)
    print("  event types:", dict(types))
    print("  turn.completed:", turns, " usages:", usages)
    print("  completed commands:", ncmd)
    print("  total command-output chars: %,d" % outbytes if False else "  total command-output chars: {:,}".format(outbytes))
    print("  ~output tokens (chars/4): {:,}".format(outbytes//4))
