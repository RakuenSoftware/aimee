import json, re, glob, os, collections

# How much of each arm's tool output is REPEATED work -- the same command, or a
# command whose output we have already seen. Repetition is pure waste: the answer
# is already in the conversation and is being re-sent regardless.
for arm in ("baseline", "aimee"):
    dup_cmd = 0; dup_out = 0; tot = 0; tot_out = 0
    seen_cmd = {}
    seen_out = {}
    per_task = collections.Counter()
    for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/%s__*__r1.jsonl" % arm)):
        task = os.path.basename(f).split("__")[1]
        seen_cmd.clear(); seen_out.clear()
        for line in open(f, errors="replace"):
            try: d = json.loads(line)
            except Exception: continue
            if d.get("type") != "item.completed": continue
            it = d.get("item") or {}
            if (it.get("item_type") or it.get("type")) != "command_execution": continue
            c = re.sub(r'\s+', ' ', (it.get("command") or "")).strip()
            o = it.get("aggregated_output") or ""
            tot += 1; tot_out += len(o)
            if c in seen_cmd:
                dup_cmd += 1; dup_out += len(o); per_task[task] += len(o)
            elif o and o in seen_out:
                dup_out += len(o); per_task[task] += len(o)
            seen_cmd[c] = 1
            if o: seen_out[o] = 1
    print("=== %s ===" % arm)
    print("  commands: %d   total output: %s" % (tot, format(tot_out, ",")))
    print("  exact-duplicate commands: %d" % dup_cmd)
    print("  output re-delivered (dup cmd or identical output): %s  (%.1f%%)"
          % (format(dup_out, ","), 100.0 * dup_out / tot_out if tot_out else 0))
    for t, v in per_task.most_common(4):
        print("     %-16s %s chars" % (t, format(v, ",")))
