set -u
T=am_12b43fa38e
W=/tmp/rp-$T
rm -rf "$W"; cp -a /opt/bench/amcorpus/corpus/$T "$W"
cd "$W"
patch -p1 --batch --forward < /opt/bench/results/cells/aimee__${T}__r1/patch.diff 2>&1 | tail -3
export PT_BUILD_TIMEOUT=2400 PYTHONPATH="$W"
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" 2>&1 | grep -E "Assertion|assert|FAILED|^OK|Ran 1|build failed" | head -8
cd /; rm -rf "$W"
