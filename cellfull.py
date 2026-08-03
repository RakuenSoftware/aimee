import json, glob, os, collections
R = collections.defaultdict(dict)
for f in sorted(glob.glob("/opt/bench/results/cells/*/summary.json")):
    d = os.path.basename(os.path.dirname(f)); arm, task, _ = d.split("__")
    s = json.load(open(f)); cx = s.get("codex", {}) or {}
    u = cx.get("usage", {}) or {}; tc = cx.get("tool_calls", {}) or {}
    it = cx.get("item_types", {}) or {}
    try: p = json.load(open(f.replace("summary","hidden")))["passed"]
    except Exception: p = None
    try:
        hj = json.load(open(f.replace("summary","hidden")))
        ec = hj.get("exit_code")
    except Exception: ec = None
    dif = ""
    pf = os.path.join(os.path.dirname(f), "patch.diff")
    if os.path.exists(pf):
        t = open(pf, errors="replace").read()
        dif = "%d+/%d-" % (sum(1 for l in t.splitlines() if l.startswith("+") and not l.startswith("+++")),
                           sum(1 for l in t.splitlines() if l.startswith("-") and not l.startswith("---")))
    R[task][arm] = dict(
        p=p, ec=ec, cr=round(cx.get("estimated_credits",0) or 0,1),
        inp=u.get("input_tokens",0), cache=u.get("cached_input_tokens",0),
        out=u.get("output_tokens",0), reas=u.get("reasoning_output_tokens",0),
        cmd=tc.get("command_execution",0), fc=tc.get("file_change",0),
        mcp=sum(v for k,v in tc.items() if k.startswith("mcp:")),
        mcpd=",".join("%s=%d"%(k.split(":")[-1],v) for k,v in sorted(tc.items()) if k.startswith("mcp:")),
        msg=it.get("agent_message",0), err=it.get("error",0),
        ev=cx.get("events",0), wall=round(s.get("wall_seconds",0) or 0), dif=dif)
ARMS = ("baseline","ponytail-instructions","ponytail-addon","aimee")
hdr = "%-22s %-4s %7s %10s %10s %7s %6s %5s %4s %4s %4s %4s %9s  %s"
print(hdr % ("arm","res","credits","input_tok","cached_tok","out_tok","reason","cmd","file","mcp","msg","err","patch","mcp detail"))
for task in sorted(R):
    print("\n== %s ==" % task)
    for a in ARMS:
        if a not in R[task]:
            print("%-22s   (not run)" % a); continue
        r = R[task][a]
        print(hdr % (a, "PASS" if r["p"] else "FAIL", r["cr"], f'{r["inp"]:,}', f'{r["cache"]:,}',
                     f'{r["out"]:,}', r["reas"], r["cmd"], r["fc"], r["mcp"], r["msg"], r["err"],
                     r["dif"], r["mcpd"] or "-"))
