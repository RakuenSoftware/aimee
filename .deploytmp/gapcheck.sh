set -u
# For each solvable-but-all-fail task: what did aimee actually produce, and how
# does it differ in SHAPE from the reference? Files touched is the first cut --
# a fix that never touched a file the reference had to change cannot pass.
for T in am_12b43fa38e am_b84c9294aa am_1e7cb3da16; do
  D=/opt/bench/results/cells/aimee__${T}__r1
  echo "=== $T ==="
  [ -f "$D/patch.diff" ] || { echo "  no patch artifact"; continue; }
  echo "  aimee LOC: $(python3 -c "
import json;d=json.load(open('$D/summary.json'));l=d.get('loc') or {}
print('added',l.get('added'),'files',l.get('files'))" 2>/dev/null)"
  echo "  aimee touched:"
  grep -E '^\+\+\+ ' "$D/patch.diff" 2>/dev/null | sed 's|^+++ b/|      |' | head -8
  echo "  reference touched:"
  grep -E '^\+\+\+ ' /opt/bench/refpatches/${T}.patch 2>/dev/null | sed 's|^+++ b/|      |' | head -8
done
echo GAPCHECK_DONE
