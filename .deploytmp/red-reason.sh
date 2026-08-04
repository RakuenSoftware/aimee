set -u
T=am_4aec72896d
W=/tmp/red-$T
rm -rf "$W"; cp -a /opt/bench/amcorpus/corpus/$T "$W"
export PT_BUILD_TIMEOUT=2400 PYTHONPATH="$W"
cd "$W"
timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" 2>&1 | head -30
cd /; rm -rf "$W"
