import json, collections, sys

# Full per-arm accounting for one task: tokens (in / out / cached), call counts
# split shell vs MCP, and total tool-output bytes. The cost model is BASE-per-call
# plus resent output, so calls and outchars are the two levers -- print both.
T = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"
for arm in ["baseline", "ponytail-instructions", "ponytail-addon", "aimee"]:
    f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (arm, T)
    tin = tout = tcache = 0.0
    cmds = calls = outchars = 0
    mcp = collections.Counter()
    try:
        fh = open(f, errors="replace")
    except Exception:
        print("%-22s MISSING" % arm)
        continue
    for line in fh:
        try:
            d = json.loads(line)
        except Exception:
            continue
        u = d.get("usage") or (d.get("item") or {}).get("usage")
        if isinstance(u, dict):
            tin += u.get("input_tokens", 0) or 0
            tout += u.get("output_tokens", 0) or 0
            tcache += u.get("cached_input_tokens", 0) or 0
        if d.get("type") != "item.completed":
            continue
        it = d.get("item") or {}
        ty = it.get("item_type") or it.get("type")
        if ty in ("command_execution", "local_shell_call"):
            cmds += 1
            calls += 1
        if ty == "mcp_tool_call":
            calls += 1
            n = it.get("tool") or it.get("name") or "?"
            mcp[n.split("__")[-1]] += 1
        o = it.get("aggregated_output") or it.get("output") or ""
        outchars += len(o) if isinstance(o, str) else len(json.dumps(o))
    print("%-22s in=%-13s out=%-8s cached=%-13s calls=%-4s shell=%-4s mcp=%-4s outchars=%s" % (
        arm, format(int(tin), ","), format(int(tout), ","), format(int(tcache), ","),
        calls, cmds, sum(mcp.values()), format(outchars, ",")))
    if mcp:
        print("      mcp:", ", ".join("%s=%d" % (k, v) for k, v in mcp.most_common()))
