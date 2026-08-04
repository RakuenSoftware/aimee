set -u
TSV=/opt/bench/amcorpus/arms/tasks.tsv
cp -a "$TSV" "$TSV.bak-accept"
python3 - "$TSV" <<'PY'
import sys, io

path = sys.argv[1]
TASK = "am_4aec72896d"

# Same level as the other tickets: observed failure plus diagnosis, no file names
# and no code. It DOES say a second service has the same defect -- that is scope,
# not location, and matches how am_12b43fa38e names "two bugs" without saying
# where either lives. Finding both sites is the task.
NEW = (
    "`aimee workspace add` never returned. The server had already answered and closed its copy of "
    "the connection, yet the client sat in read() for 28 minutes. The CLI reads until EOF, and EOF "
    "arrives only once EVERY copy of the socket is closed -- and the server forks constantly (git, "
    "delegates, deploys, process management), so any child forked while a request was in flight "
    "inherited that request's descriptor and pinned it open for the child's whole lifetime. Three "
    "benchmark lanes stalled on this, each on its first cell; short requests hide it, because the "
    "window has to overlap a fork. The same defect is present on the knowledge service's "
    "mutually-authenticated listener -- the one carrying server-to-KB traffic, which forks a "
    "curator sidecar per symbol -- while its plaintext sibling was already correct. Note that "
    "setting the flag after accepting is not sufficient in a threaded forking server: another "
    "thread can fork in the window between the two calls."
)

lines = io.open(path, encoding="utf-8").read().splitlines()
if any(l.startswith(TASK + "\t") for l in lines):
    print("  already present")
    sys.exit(0)
lines.append(TASK + "\t" + NEW)
io.open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("  added %s (%d chars)" % (TASK, len(NEW)))
PY
awk -F"\t" '{printf "  %-16s %4d\n", $1, length($2)}' "$TSV"
echo ACCEPT_TICKET_ADDED
