set -u
# Does the REFERENCE patch pass each task's own graded test? If it does not, the
# task is unsolvable by construction and its arm results measure nothing.
for T in am_12b43fa38e am_b84c9294aa am_1e7cb3da16; do
  W=/tmp/rg-$T
  rm -rf "$W"; cp -a /opt/bench/amcorpus/corpus/$T "$W" 2>/dev/null || { echo "$T: NO CORPUS"; continue; }
  cd "$W"
  patch -p1 --batch --forward < /opt/bench/refpatches/$T.patch >/dev/null 2>&1
  export PT_BUILD_TIMEOUT=2400 PYTHONPATH="$W"
  R=$(timeout 2600 python3 -m unittest discover -s /opt/bench/amcorpus/hidden -p "${T}.py" 2>&1 | grep -E "^OK|^FAILED|Assertion|build failed" | head -2 | tr '\n' ' ')
  echo "$T: $R"
  cd /; rm -rf "$W"
done
echo REFGATE_DONE
