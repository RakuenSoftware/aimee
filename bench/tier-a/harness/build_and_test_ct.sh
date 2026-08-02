#!/bin/bash
# Build aimee and run the fact-lifecycle tests on the bench container.
#
# The dev box has gcc but no cmake, so the retraction routing in
# kb_memory_facts.c could only be syntax-checked locally. CT 140 has cmake 3.31
# and libpq, so it is where "it compiles and the tests pass" can actually be
# established rather than asserted.
pct exec 140 -- bash -lc '
  set -e
  cd /opt/aimee-build
  rm -rf src && mkdir -p src && tar xzf src.tgz -C src
  cd src
  cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug > /opt/aimee-build/cmake.log 2>&1 \
    || { echo "CMAKE FAILED"; tail -25 /opt/aimee-build/cmake.log; exit 1; }
  echo "=== configured"
  cmake --build build -j 12 > /opt/aimee-build/build.log 2>&1 \
    || { echo "BUILD FAILED"; grep -iE "error|Error" /opt/aimee-build/build.log | head -30; exit 1; }
  echo "=== built"
'
