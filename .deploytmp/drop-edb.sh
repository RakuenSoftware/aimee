set -u
TSV=/opt/bench/amcorpus/arms/tasks.tsv
cp -a "$TSV" "$TSV.bak-dropedb"
# Unsolvable by construction: the reference patch fails this task's own graded
# test on an unrelated assertion (test_parent_write_guard_blocks_parent_writes)
# that needs a git repo with a valid HEAD the grading sandbox does not provide.
# No fix can pass it, so its arm results measure nothing.
grep -v '^am_edb3594485	' "$TSV" > "$TSV.tmp" && mv "$TSV.tmp" "$TSV"
awk -F"\t" '{printf "  %-16s %4d\n", $1, length($2)}' "$TSV"
echo DROPPED_EDB
