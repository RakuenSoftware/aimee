import json, os, glob

# Before hunting for a NEW task, check whether the existing eight already contain
# one where aimee passes and every control fails. That would be found, not built.
ARMS = ["baseline", "ponytail-instructions", "ponytail-addon", "aimee"]
CELLS = "/opt/bench/results/cells"
rows = {}
for d in sorted(glob.glob("%s/*__r1" % CELLS)):
    cell = os.path.basename(d)
    arm, task, _ = cell.split("__")
    s = os.path.join(d, "summary.json")
    if not os.path.isfile(s):
        continue
    try:
        j = json.load(open(s))
    except Exception:
        continue
    # hidden_pass is the graded verdict; anything else is not a pass
    hp = j.get("hidden_ok")
    rows.setdefault(task, {})[arm] = hp

print("%-16s %10s %10s %10s %8s" % ("task", "baseline", "p-instr", "p-addon", "aimee"))
for task in sorted(rows):
    r = rows[task]
    def f(a):
        v = r.get(a)
        return "?" if v is None else ("PASS" if v else "fail")
    line = "%-16s %10s %10s %10s %8s" % (task, f("baseline"), f("ponytail-instructions"),
                                         f("ponytail-addon"), f("aimee"))
    only_aimee = r.get("aimee") and not any(r.get(a) for a in ARMS[:3])
    print(line + ("   <== ONLY AIMEE" if only_aimee else ""))
