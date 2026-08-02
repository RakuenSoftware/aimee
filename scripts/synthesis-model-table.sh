#!/bin/sh
# The bundled synthesis models, and only here.
#
# Each model needs TWO files, which is not obvious and cost a wrong assumption once:
#
#   <model>-<quant>.gguf   the model
#   mtp-<model>.gguf       its multi-token-prediction draft, ~95 MB
#
# llama.cpp reaches MTP through its speculative-decoding path, so the draft is a
# SEPARATE artefact resolved from the same repo (-hfd). It is not a head inside the
# main GGUF. An image carrying only the main file starts fine, serves correctly, and
# is 1.6-1.8x slower with nothing to indicate why.
#
# THE QUANT IS PER MODEL, not one global. E2B ships Q4 because it is the small-box
# option and 1.4 GB matters there; E4B ships Q6 because it is the quality option.
#
# THE SHA256 IS THE POINT. It is Hugging Face's own LFS oid, which is also the name
# HF gives the blob in its cache -- so a baked cache is self-verifying: the filename
# in blobs/ IS the digest. It catches a truncated copy, which a length check does not
# (downloaders preallocate), and it is what makes the whole "specific, baked-in model
# we do not have to worry about changing" property checkable rather than asserted.
#
# Bumping a quant or model means bumping these, from
#   https://huggingface.co/api/models/<repo>/paths-info/main
#
# Usage:
#   synthesis-model-table.sh repo   <model-id>   -> the HF repo
#   synthesis-model-table.sh quant  <model-id>   -> the quant tag
#   synthesis-model-table.sh sha    <model-id>   -> main file sha256
#   synthesis-model-table.sh mtpsha <model-id>   -> MTP draft sha256
set -eu

what=${1:?usage: synthesis-model-table.sh <repo|quant|sha|mtpsha|files> <model-id>}
model=${2:?usage: synthesis-model-table.sh <repo|quant|sha|mtpsha|files> <model-id>}

case "$model" in
  gemma-4-E2B-it)
    repo=unsloth/gemma-4-E2B-it-GGUF
    quant=UD-Q4_K_XL
    sha=b52f438017efaec5debf1c0d8be690571e212a07c312f1102bbce927258cfc32
    mtpsha=9eba819938efccfd6044f8af84e3bbfddc639a2bcf32ebc36420e6a649191919 ;;
  gemma-4-E4B-it)
    repo=unsloth/gemma-4-E4B-it-GGUF
    quant=UD-Q6_K_XL
    sha=17b9c459b28b420ce20d75bcfc329db4fac1343792a964c3ae2e2680ce768932
    mtpsha=b6a723115efa510d3b3215db1e26790dae84cd08c2134a764f3d194f1f0c3376 ;;
  *)
    echo "synthesis model must be gemma-4-E2B-it or gemma-4-E4B-it (got '$model')" >&2
    exit 1 ;;
esac

case "$what" in
  repo)   echo "$repo" ;;
  quant)  echo "$quant" ;;
  sha)    echo "$sha" ;;
  mtpsha) echo "$mtpsha" ;;
  files)  echo "${model}-${quant}.gguf mtp-${model}.gguf" ;;
  *) echo "unknown field '$what'" >&2; exit 1 ;;
esac
