import json, re, glob, os, collections
shell_git = collections.Counter(); mcp_git = collections.Counter(); cmds = collections.Counter()
for f in sorted(glob.glob("/var/lib/aimee-workspaces/bench/raw/aimee__*__r1.jsonl")):
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        t = it.get("item_type") or it.get("type")
        if t == "mcp_tool_call":
            name = it.get("tool") or it.get("name") or ""
            if "git" in name: mcp_git[name] += 1
        elif t == "command_execution":
            c = re.sub(r'^/bin/bash\s+-lc\s+', '', it.get("command") or "", flags=re.S)
            for frag in re.split(r'&&|\|\||;', c):
                m = re.search(r'\bgit\s+(-C\s+\S+\s+)?([a-z-]+)', frag)
                if m:
                    shell_git[m.group(2)] += 1
print("aimee arm — shell `git` subcommands used:")
for k, v in shell_git.most_common(12): print("   %-16s %d" % (k, v))
print("   TOTAL shell git fragments:", sum(shell_git.values()))
print("aimee arm — MCP git tool calls:", dict(mcp_git) or "NONE")
