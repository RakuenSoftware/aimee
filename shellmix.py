import json, collections, sys, re

# aimee runs MORE shell commands than baseline while also making 25 MCP calls.
# If the MCP retrieval were replacing shell exploration, shell should FALL. Print
# every shell command verbatim per arm so the extra 13 can be identified.
T = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"
for arm in ["baseline", "aimee"]:
    f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (arm, T)
    print("================ %s ================" % arm)
    verbs = collections.Counter()
    n = 0
    try:
        fh = open(f, errors="replace")
    except Exception:
        print("  MISSING")
        continue
    for line in fh:
        try:
            d = json.loads(line)
        except Exception:
            continue
        if d.get("type") != "item.completed":
            continue
        it = d.get("item") or {}
        if (it.get("item_type") or it.get("type")) not in ("command_execution", "local_shell_call"):
            continue
        cmd = it.get("command") or ""
        if isinstance(cmd, list):
            cmd = " ".join(cmd)
        cmd = re.sub(r"\s+", " ", cmd).strip()
        if cmd.startswith("bash -lc "):
            cmd = cmd[9:].strip("'\"")
        n += 1
        out = it.get("aggregated_output") or it.get("output") or ""
        olen = len(out) if isinstance(out, str) else len(json.dumps(out))
        print("  %2d [%6d ch] %s" % (n, olen, cmd[:150]))
        for w in re.findall(r"\b(rg|grep|sed|cat|ls|find|make|git|python3|pytest|head|tail|awk|apply_patch|nl|wc)\b", cmd):
            verbs[w] += 1
    print("  verbs:", ", ".join("%s=%d" % (k, v) for k, v in verbs.most_common()))
    print()
