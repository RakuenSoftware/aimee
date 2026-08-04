set -u
T=am_edb3594485
W=/tmp/ref-$T
rm -rf "$W"; cp -a /opt/bench/amcorpus/corpus/$T "$W"
cd "$W"
patch -p1 --batch --forward < /opt/bench/refpatches/$T.patch 2>&1 | tail -2
export PT_BUILD_TIMEOUT=2400 PYTHONPATH="$W"
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" 2>&1 | grep -E "Assertion|FAILED|^OK|Ran 1" | tail -6
cd /; rm -rf "$W"
