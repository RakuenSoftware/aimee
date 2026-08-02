#!/bin/sh
# Pull each published image from ghcr and prove it is what it claims to be.
#
# The workflow reporting "success" is not the same as an image an operator can
# pull and run — that gap is the whole theme of this work. So: pull the manifest,
# check it really is multi-arch, and read back the identity the image records.
set -u
TAG=${1:-sha-c1d0aba}
REG=ghcr.io/rakuensoftware

# variant | expect_llama | expect_model
V='aimee-kb|0|
aimee-kb-nomic|0|
aimee-kb-llm-e2b|1|gemma-4-E2B-it
aimee-kb-llm-e4b|1|gemma-4-E4B-it
aimee-kb-nomic-llm-e2b|1|gemma-4-E2B-it
aimee-kb-nomic-llm-e4b|1|gemma-4-E4B-it'

pass=0; fail=0
for row in $V; do
  name=$(echo "$row" | cut -d'|' -f1)
  wl=$(echo "$row"   | cut -d'|' -f2)
  wm=$(echo "$row"   | cut -d'|' -f3)
  ref="$REG/$name:$TAG"

  # multi-arch manifest, without pulling gigabytes
  arches=$(docker manifest inspect "$ref" 2>/dev/null \
           | grep -o '"architecture": *"[a-z0-9]*"' | grep -oE '(amd64|arm64)' | sort -u | tr '\n' ',' )
  case "$arches" in
    *amd64*arm64*) echo "  PASS $name: manifest is multi-arch ($arches)"; pass=$((pass+1)) ;;
    *) echo "  FAIL $name: arches=${arches:-none}"; fail=$((fail+1)); continue ;;
  esac

  if ! docker pull -q "$ref" >/dev/null 2>&1; then
    echo "  FAIL $name: pull failed"; fail=$((fail+1)); continue
  fi
  gl=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$ref" 2>/dev/null \
       | grep '^AIMEE_WITH_LLAMACPP=' | cut -d= -f2)
  gm=$(docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$ref" 2>/dev/null \
       | grep '^AIMEE_SYNTHESIS_MODEL=' | cut -d= -f2)
  if [ "$gl" = "$wl" ] && [ "$gm" = "$wm" ]; then
    echo "  PASS $name: pulled, llamacpp=$gl model='${gm}'"; pass=$((pass+1))
  else
    echo "  FAIL $name: llamacpp=$gl want $wl, model='$gm' want '$wm'"; fail=$((fail+1))
  fi
done
echo "==== ghcr verify: $pass passed, $fail failed ===="
[ "$fail" -eq 0 ]
