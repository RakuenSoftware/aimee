#!/bin/sh
# Verify the mirrored GGUFs against Hugging Face's own sha256 (the LFS oid).
#
# The share is being written by another process, so a file can be the RIGHT SIZE
# and the WRONG CONTENT — many downloaders preallocate the full length and fill it
# in. A size check cannot see that; a hash can.
set -u
E2B_WANT=ae15474bc78f68c6a44bd17cad32f672b9501d90c4a0eed2fceeb6878ed530c5
E4B_WANT=17b9c459b28b420ce20d75bcfc329db4fac1343792a964c3ae2e2680ce768932

check() {
  f=$1; want=$2; name=$3
  [ -f "$f" ] || { echo "$name: MISSING"; return 1; }
  s1=$(stat -c %s "$f"); sleep 20; s2=$(stat -c %s "$f")
  if [ "$s1" != "$s2" ]; then
    echo "$name: STILL GROWING ($s1 -> $s2) — in-flight, do not use"; return 1
  fi
  got=$(sha256sum "$f" | cut -d' ' -f1)
  if [ "$got" = "$want" ]; then
    echo "$name: OK sha256=$got"
  else
    echo "$name: MISMATCH got=$got want=$want"; return 1
  fi
}

rc=0
check /mnt/gguf/gemma-4-E2B-it-UD-Q6_K_XL.gguf "$E2B_WANT" E2B || rc=1
check /mnt/gguf/gemma-4-E4B-it-UD-Q6_K_XL.gguf "$E4B_WANT" E4B || rc=1
echo "hashcheck-exit=$rc"
exit $rc
