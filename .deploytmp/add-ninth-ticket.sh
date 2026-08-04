set -u
TSV=/opt/bench/amcorpus/arms/tasks.tsv
cp -a "$TSV" "$TSV.bak-ninth"
python3 - "$TSV" <<'PY'
import sys, io

path = sys.argv[1]
TASK = "am_edb3594485"

# Written from the reference patch at the SAME level as the other eight: observed
# failure plus diagnosis, no code and no file names. Deliberately does NOT say
# where the discard happens -- locating that is the task, and it is the part a
# text search cannot do, because the message the operator sees is emitted in a
# different file from the defect and shares no literal with it.
NEW = (
    "A codex delegate returned nothing on every attempt: the turn completed, but the delegate "
    "reported no content in the final response with tool_calls=0. On the wire the model had in "
    "fact emitted a tool call -- the item types streamed for that turn were reasoning followed by "
    "function_call -- so the call was made and then lost between arriving and being read. The "
    "streaming path banks each completed output item as it arrives, but the object it hands on is "
    "built from the terminal completed payload, and when that payload carries no function_call of "
    "its own the banked ones are discarded rather than folded back in. The warning that fires in "
    "this case also counted only reasoning items, so it reported items unaccounted for without "
    "ever naming what had actually arrived."
)

lines = io.open(path, encoding="utf-8").read().splitlines()
if any(l.startswith(TASK + "\t") for l in lines):
    print("  already present; leaving as-is")
    sys.exit(0)
lines.append(TASK + "\t" + NEW)
io.open(path, "w", encoding="utf-8").write("\n".join(lines) + "\n")
print("  added %s (%d chars)" % (TASK, len(NEW)))
PY
echo "--- tasks now ---"
awk -F"\t" '{printf "  %-16s %4d chars\n", $1, length($2)}' "$TSV"
echo NINTH_ADDED
