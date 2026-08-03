import json, glob, os

# Solve for the chars-per-token that makes the resend model agree with the
# MEASURED input_tokens. If it lands near 2.5-3 (typical for code/JSON) then the
# "unattributed" gap was my chars/4 heuristic, not a hidden cost.
TOTALS = {
    "baseline__am_312e901904": 3852870, "aimee__am_312e901904": 3734958,
    "baseline__am_e1af40a0f5": 441735,  "aimee__am_e1af40a0f5": 1245093,
    "baseline__am_1f0f1ab528": 491463,  "aimee__am_1f0f1ab528": 1144961,
}
BASE_PER_CALL = 12300   # measured directly by the isolation run

for cell, total in sorted(TOTALS.items()):
    f = "/var/lib/aimee-workspaces/bench/raw/%s__r1.jsonl" % cell
    if not os.path.exists(f):
        continue
    outs = []
    for line in open(f, errors="replace"):
        try: d = json.loads(line)
        except Exception: continue
        if d.get("type") != "item.completed": continue
        it = d.get("item") or {}
        k = it.get("item_type") or it.get("type")
        if k == "command_execution":
            outs.append(len(it.get("aggregated_output") or ""))
        elif k in ("mcp_tool_call", "agent_message"):
            outs.append(len(json.dumps(it)))
    n = len(outs) + 1
    # accumulated CHARS re-sent across calls
    acc_chars = 0; prefix = 0
    for c in outs:
        acc_chars += prefix
        prefix += c
    acc_chars += prefix
    budget = total - BASE_PER_CALL * n          # tokens left for accumulated content
    cpt = acc_chars / float(budget) if budget > 0 else float("nan")
    print("%-28s calls=%3d  base=%9s  acc_chars=%12s  implied chars/token=%.2f"
          % (cell, n, format(BASE_PER_CALL * n, ","), format(acc_chars, ","), cpt))
