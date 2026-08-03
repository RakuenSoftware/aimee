import json, re, sys

TASK = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"
for arm in ("baseline", "aimee"):
    f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (arm, TASK)
    print("=== %s ===" % arm)
    n = 0
    try:
        lines = open(f, errors="replace").read().splitlines()
    except FileNotFoundError:
        print("  (no transcript)"); continue
    for line in lines:
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "command_execution": continue
        c = re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)
        out = len(it.get("aggregated_output") or "")
        # isolate the rg/grep fragments of each (possibly compound) command
        for frag in re.split(r'&&|\|\||;', c):
            frag = frag.strip()
            if re.match(r'^["\']?\s*(rg|grep)\b', frag) and "--files" not in frag:
                n += 1
                print("  [%6d chars out] %s" % (out, frag[:120]))
    print("  total search fragments:", n)
    print()
