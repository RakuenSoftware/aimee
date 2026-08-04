set -u
T=am_4aec72896d
C=/opt/bench/amcorpus/corpus/$T
W=/tmp/val-$T
rm -rf "$W"; cp -a "$C" "$W"
cd "$W"
export PT_BUILD_TIMEOUT=2400
export PYTHONPATH="$W"
echo "=== RED: pristine corpus must FAIL the graded test ==="
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" -v 2>&1 | tail -6
echo "=== apply reference patch ==="
git init -q . 2>/dev/null; git apply --stat /opt/bench/refpatches/$T.patch 2>&1 | tail -3
patch -p1 --batch --forward < /opt/bench/refpatches/$T.patch 2>&1 | tail -4
echo "=== GREEN: patched corpus must PASS ==="
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" -v 2>&1 | tail -6
cd /; rm -rf "$W"
echo VALIDATE_DONE
