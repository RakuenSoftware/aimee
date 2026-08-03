import json, re, glob, os

rows = []
for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/*__r1.jsonl")):
    cell = os.path.basename(f)[:-6]
    arm = cell.split("__")[0]
    mtime = os.path.getmtime(f)
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) != "command_execution": continue
        out = len(it.get("aggregated_output") or "")
        c = re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)
        rows.append((out, arm, cell.split("__")[1], mtime, c.replace("\n", " ")[:110]))

rows.sort(reverse=True)
import time
print("%9s %-10s %-16s %-6s %s" % ("chars", "arm", "task", "when", "command"))
for out, arm, task, mt, c in rows[:14]:
    print("%9s %-10s %-16s %-6s %s" % (format(out, ","), arm, task,
                                       time.strftime("%H:%M", time.localtime(mt)), c))
