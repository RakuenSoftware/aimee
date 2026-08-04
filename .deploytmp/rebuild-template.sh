set -e
T=/var/lib/aimee-workspaces/bench/templates
# Templates are prepared once and reused for every cell. A stale template served
# four CLI installs' worth of cells earlier today and voided an entire A/B series,
# so blow the aimee template away and let the next run regenerate it from the
# binary that is actually installed.
rm -rf "$T/aimee"
echo "removed: $T/aimee"
# Regenerate now rather than at run time, so the contents can be checked before a
# cell is spent on them.
/usr/local/bin/aimee client install codex --home "$T/aimee/codex-home" 2>&1 | tail -3 || true
echo "--- generated tree ---"
find "$T/aimee" -maxdepth 6 \( -name 'SKILL.md' -o -name 'codex-hooks.json' \) 2>/dev/null
echo REBUILD_DONE
