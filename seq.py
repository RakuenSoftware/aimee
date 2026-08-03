import json, sys, re, collections

# Why does ONE task cost 2.6x when three others come in under 1.0x? Dump the full
# interleaved call sequence -- shell and MCP together, in order, with the output
# size each one returned -- so a turn that bought nothing is visible as a turn
# rather than hidden inside an aggregate.
T = sys.argv[1] if len(sys.argv) > 1 else "am_e1af40a0f5"
ARM = sys.argv[2] if len(sys.argv) > 2 else "aimee"
f = "/var/lib/aimee-workspaces/bench/raw/%s__%s__r1.jsonl" % (ARM, T)

n = 0
empty = 0
tot = 0
by_kind = collections.Counter()
for line in open(f, errors="replace"):
    try:
        d = json.loads(line)
    except Exception:
        continue
    if d.get("type") != "item.completed":
        continue
    it = d.get("item") or {}
    ty = it.get("item_type") or it.get("type")
    if ty in ("command_execution", "local_shell_call"):
        kind = "sh"
        cmd = it.get("command") or ""
        if isinstance(cmd, list):
            cmd = " ".join(cmd)
        cmd = re.sub(r"\s+", " ", cmd).strip()
        if cmd.startswith("/bin/bash -lc"):
            cmd = cmd[13:].strip().strip("'\"")
    elif ty == "mcp_tool_call":
        name = (it.get("tool") or it.get("name") or "?").split("__")[-1]
        args = it.get("arguments") or it.get("input") or {}
        if isinstance(args, str):
            try:
                args = json.loads(args)
            except Exception:
                args = {}
        kind = "MCP"
        bits = [str(args.get(k)) for k in ("command", "query", "symbol", "file_path")
                if args.get(k)]
        rng = ""
        if args.get("line_start"):
            rng = ":%s-%s" % (args.get("line_start"), args.get("line_end"))
        cmd = "%s(%s%s)" % (name, " ".join(bits)[:70], rng)
    else:
        continue
    out = it.get("aggregated_output") or it.get("output") or ""
    olen = len(out) if isinstance(out, str) else len(json.dumps(out))
    n += 1
    tot += olen
    by_kind[kind] += 1
    if olen < 120:
        empty += 1
    print("%3d %-3s [%7d] %s" % (n, kind, olen, cmd[:130]))

print()
print("%s / %s: %d calls (%s), %s output chars, %d calls returned <120 chars" %
      (ARM, T, n, ", ".join("%s=%d" % kv for kv in by_kind.most_common()),
       format(tot, ","), empty))
