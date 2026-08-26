#!/bin/sh
set -eu

binary=${1:?usage: check-binary-hardening.sh BINARY}
fail=0
check() {
  if ! eval "$2"; then
    echo "hardening-check: missing $1 in $binary" >&2
    fail=1
  fi
}

check PIE "readelf -h '$binary' | grep -q 'Type:.*DYN'"
check full-RELRO "readelf -l '$binary' | grep -q GNU_RELRO && readelf -d '$binary' | grep -q BIND_NOW"
check NX-stack "! readelf -W -l '$binary' | grep 'GNU_STACK' | grep -q 'RWE'"
check stack-canary "readelf -Ws '$binary' | grep -q __stack_chk_fail"
check fortified-libc "readelf -Ws '$binary' | grep -Eq '__[^[:space:]]+_chk@'"
test "$fail" -eq 0
echo "hardening-check: PIE, full RELRO, NX, canary, and fortified libc verified"
