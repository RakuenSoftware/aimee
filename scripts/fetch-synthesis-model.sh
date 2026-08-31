#!/bin/sh
# Fetch one bundled synthesis model into <outdir>/synthesis.gguf, verified.
#
# Used by BOTH Dockerfile (the inline fetch fallback) and Dockerfile.model (the
# prebuilt model image). The repo/filename/digest table lives here and only here:
# it was duplicated once and the two copies are exactly the kind of thing that
# drifts silently, since a stale digest does not fail the build that carries it —
# it fails whichever build still trusts the old one.
#
# THE SHA256 IS THE POINT, NOT A FORMALITY. It is Hugging Face's own digest for
# the file (the LFS oid), and it is what makes AIMEE_MODEL_MIRROR safe to offer:
# a mirror is an untrusted input, so anyone who can write to one could otherwise
# change which model your image runs. It also catches the ordinary failure — a
# truncated or half-written copy. A LENGTH check does not: plenty of downloaders
# preallocate the full size and fill it in, so a file can be exactly the right
# number of bytes and entirely wrong. A digest is the only check that sees that.
#
# Bumping the quant or the model means bumping these, from
# https://huggingface.co/api/models/<repo>?blobs=true (siblings[].lfs.oid).
#
# An explicit repo+filename per model id: no quant-tag resolution at runtime,
# which is what silently served the wrong file when a tag stopped matching.
# The UD (Unsloth Dynamic) quants are published only in unsloth's repos.
#
# THE TWO ENTRIES ARE DELIBERATELY NOT THE SAME QUANT, and E2B is deliberately the
# QAT checkpoint: see the header of scripts/synthesis-model-table.sh, which carries
# the reasoning and the measurement. KEEP THE TWO TABLES IN STEP. They serve
# different builds -- that one Dockerfile.llm, this one Dockerfile.model -- and they
# have drifted before: this entry sat on a non-QAT UD-Q6_K_XL E2B while the other
# had moved to UD-Q4_K_XL, so the model image and the sidecar image disagreed about
# which weights "gemma-4-E2B-it" names.
set -eu

model=${1:?usage: fetch-synthesis-model.sh <model-id> <outdir>}
outdir=${2:?usage: fetch-synthesis-model.sh <model-id> <outdir>}
mirror=${AIMEE_MODEL_MIRROR:-}

case "$model" in
  gemma-4-E2B-it)
    repo=unsloth/gemma-4-E2B-it-qat-GGUF; file=gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf
    sha=e531007218dfab990486a5de7676a6932d6ea8dea233d1f698d7c21cf8a16889 ;;
  gemma-4-E4B-it)
    repo=unsloth/gemma-4-E4B-it-GGUF; file=gemma-4-E4B-it-UD-Q6_K_XL.gguf
    sha=17b9c459b28b420ce20d75bcfc329db4fac1343792a964c3ae2e2680ce768932 ;;
  *)
    echo "synthesis model must be gemma-4-E2B-it or gemma-4-E4B-it (got '$model')" >&2
    exit 1 ;;
esac

if [ -n "$mirror" ]; then url="${mirror%/}/${file}"
else url="https://huggingface.co/${repo}/resolve/main/${file}"; fi

mkdir -p "$outdir"

# RETRY, and span minutes rather than seconds. Baking the model moves the Hugging
# Face dependency off the user's first run and onto our build, which is the right
# trade — we control builds, users do not — but the build then has to survive HF
# being slow or rate-limiting. A flat `--retry 5 --retry-delay 5` is a ~25-second
# burst and it has already lost a CI leg to exit 22 while the same commit built
# fine minutes later. -C - resumes a partial transfer rather than restarting
# gigabytes; the digest check below is what makes a resumed file safe to trust.
#
# -sS: silent, but still reports errors. Without it curl writes a progress meter
# on every buffer flush, thousands of lines of noise in a non-tty build log.
a=1
while [ "$a" -le 6 ]; do
  if curl -fL -sS --retry 3 --retry-delay 5 --retry-all-errors \
          --connect-timeout 20 -C - -o "$outdir/synthesis.gguf" "$url"; then
    break
  fi
  echo "model fetch failed (attempt $a/6); backing off" >&2
  sleep $((a * 20))
  a=$((a + 1))
done

echo "${sha}  ${outdir}/synthesis.gguf" | sha256sum -c -
echo "$model" > "$outdir/MODEL_ID"
