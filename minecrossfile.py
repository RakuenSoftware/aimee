import subprocess, re, collections, sys

# Find fix commits whose SYMPTOM and CAUSE live in different files with no shared
# literal. That is the shape an index answers and a text search cannot: grepping
# the operator-visible message lands you in the file that PRINTS it, which is not
# the file that has to change.
#
# Heuristic: a commit touching >=2 source files, where at least one file's diff
# only touches log/warn/error text (the symptom side) and another changes real
# logic (the cause side). Rank by how far apart the files are in the tree.
LOOKBACK = sys.argv[1] if len(sys.argv) > 1 else "400"

log = subprocess.run(["git", "log", "--format=%H|%s", "-n", LOOKBACK],
                     capture_output=True, text=True).stdout.splitlines()

cands = []
for line in log:
    if "|" not in line:
        continue
    sha, subj = line.split("|", 1)
    if not re.match(r"^(fix|perf)", subj, re.I):
        continue
    stat = subprocess.run(["git", "show", "--format=", "--name-only", sha],
                          capture_output=True, text=True).stdout.split()
    src = [f for f in stat if f.endswith((".c", ".h", ".go", ".py")) and "/tests/" not in f
           and not f.startswith("src/tests/")]
    if not (2 <= len(src) <= 6):
        continue
    diff = subprocess.run(["git", "show", "--format=", sha], capture_output=True,
                          text=True).stdout
    # split the diff per file
    per, cur = {}, None
    for l in diff.splitlines():
        if l.startswith("+++ b/"):
            cur = l[6:]
            per[cur] = []
        elif cur and (l.startswith("+") or l.startswith("-")) and not l.startswith(("+++", "---")):
            per[cur].append(l)
    msg_only, logic = [], []
    for f, lines in per.items():
        if f not in src or not lines:
            continue
        touched = "\n".join(lines)
        has_log = re.search(r"LOG_(WARN|ERROR|INFO)|aimee_log|fprintf\(stderr", touched)
        # "message only" = every changed line is inside a log/■string context
        code_lines = [l for l in lines if not re.search(
            r'LOG_|aimee_log|"|\*|^\s*[+-]\s*$', l)]
        if has_log and len(code_lines) <= 2:
            msg_only.append(f)
        elif len(code_lines) >= 3:
            logic.append(f)
    if msg_only and logic:
        # distance: different top-level dirs is the interesting case
        d1 = {f.split("/")[1] if f.count("/") > 1 else f for f in msg_only}
        d2 = {f.split("/")[1] if f.count("/") > 1 else f for f in logic}
        cands.append((len(d1 | d2), sha[:12], subj[:70], msg_only, logic))

cands.sort(reverse=True)
print("candidates: symptom file(s) != cause file(s)\n")
for n, sha, subj, m, l in cands[:12]:
    print("%s  %s" % (sha, subj))
    print("   symptom: %s" % ", ".join(m))
    print("   cause  : %s" % ", ".join(l))
    print()
print("total: %d" % len(cands))
