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
# option and every GB matters there; E4B ships Q6 because it is the quality option.
#
# E2B ALSO SHIPS QAT WEIGHTS, and that is why its quant tag carries a `qat-` prefix
# while E4B's does not. Quantisation-aware training recovers most of what Q4 costs a
# model this small: the campaign measured google's QAT q4_0 arm at +0.0389 strict F1
# over the same model's UD-Q4_K_XL, a delta outside the +/-0.024 interval that n=1001
# resolves. That evidence is NOT on this branch -- it is defect 39 in
# bench/tier-a/MEASUREMENT_LOG.md on branch bench/tier-a-small-models (629c62eb93),
# whose copy of that file runs past the defect 30 this branch stops at. What ships here is unsloth's
# UD requant OF THAT QAT CHECKPOINT -- it keeps the repo-shaped addressing and the
# in-repo MTP draft that google's GGUF repo does not publish, and it is 2.62 GB
# against google's 3.35 GB. It has NOT been benchmarked separately; the +0.0389 is
# evidence for QAT weights, not for this exact requant of them.
#
# The `qat-` prefix lives in the QUANT rather than the model id on purpose: the model
# id is the image axis (aimee-llm-e2b) and is referenced from config, the wizard and
# the deploy layer, while `files` builds ${model}-${quant}.gguf -- which is exactly
# the published gemma-4-E2B-it-qat-UD-Q4_K_XL.gguf. The MTP draft is NOT qat-tagged
# in that repo; it is mtp-${model}.gguf, so that half of `files` is unchanged.
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
    repo=unsloth/gemma-4-E2B-it-qat-GGUF
    quant=qat-UD-Q4_K_XL
    sha=e531007218dfab990486a5de7676a6932d6ea8dea233d1f698d7c21cf8a16889
    mtpsha=586f2460b909008640981ec34060aa864e03c144fbabfb3173c4335087e4aae0 ;;
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
