set -u
T=am_edb3594485
W=/tmp/repro-$T
rm -rf "$W"; cp -a /opt/bench/amcorpus/corpus/$T "$W"
cd "$W"
patch -p1 --batch --forward < /opt/bench/results/cells/aimee__${T}__r1/patch.diff 2>&1 | tail -4
export PT_BUILD_TIMEOUT=2400 PYTHONPATH="$W"
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" 2>&1 | tail -18
cd /; rm -rf "$W"
