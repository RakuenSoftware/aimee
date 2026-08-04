set -e
TASK="${TASK:?}"
D=/opt/bench/amcorpus/corpus/$TASK
rm -rf "$D"
mkdir -p "$D"
tar -x -C "$D"
du -sh "$D"
ls "$D" | head -4
echo CORPUS_READY
