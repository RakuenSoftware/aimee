T=/var/lib/aimee-workspaces/bench/templates/aimee
S=$(find "$T" -name SKILL.md 2>/dev/null | head -1)
H=$(find "$T" -name codex-hooks.json 2>/dev/null | head -1)
echo "SKILL.md: $S"
echo "hooks:    $H"
[ -n "$S" ] || { echo "NO SKILL.md YET (template still building)"; exit 0; }
echo "--- guidance phrases actually in the served skill ---"
for p in "ONE call, joined with" "spans" "Do not read this file" "command=span" "PHRASE rather than a symbol" "Cap what a search prints" "make -s" "Do not repeat a search"; do
  printf "  %-32s %s\n" "$p" "$(grep -c "$p" "$S" 2>/dev/null)"
done
echo "--- hook registration ---"
[ -n "$H" ] && grep -o '"command": "[^"]*"' "$H" | head -4
echo VERIFY_DONE
