#!/bin/bash
# Syntax-check every validation artifact in this directory.
#
# These scripts are not exercised by CI -- they run against a container -- so a
# typo in one is found the hard way, mid-run, on a box that then has to be
# rebuilt. This is the cheap guard.
set -u
cd "$(dirname "$0")" || exit 1
rc=0
for f in *.sh; do
  bash -n "$f" || { echo "SHELL SYNTAX FAIL: $f" >&2; rc=1; }
done
for f in *.py; do
  [ -e "$f" ] || continue
  python3 -m py_compile "$f" || { echo "PYTHON SYNTAX FAIL: $f" >&2; rc=1; }
done
rm -rf __pycache__
[ $rc -eq 0 ] && echo "validation scripts: syntax ok ($(ls *.sh *.py 2>/dev/null | wc -l) files)"
exit $rc
