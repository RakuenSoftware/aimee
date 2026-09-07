#!/usr/bin/env bash
# A runtime release may publish each synthesis image only once. The caller has
# already checked the exact content tag, so this also permits idempotent retries.
set -euo pipefail
image=${1:?image repository required}
version=${2:?upstream release required}
[[ $version =~ ^b[0-9]+$ ]] || { echo 'invalid upstream runtime release' >&2; exit 1; }
release=$(gh api "repos/ggml-org/llama.cpp/releases/tags/$version" \
    --jq 'select(.draft == false and .published_at != null) | .tag_name')
[[ $release == "$version" ]] || { echo 'runtime has no published upstream release' >&2; exit 1; }
# Do not interpret authentication, rate limiting or registry outages as absence.
if detail=$(docker manifest inspect "${image}:runtime-${version}" 2>&1); then
    echo "synthesis image already published for $version; select a new upstream release" >&2
    exit 1
elif ! [[ $detail =~ (manifest\ unknown|no\ such\ manifest|MANIFEST_UNKNOWN) ]]; then
    echo 'cannot verify whether the synthesis runtime release was already published' >&2
    exit 1
fi
echo "verified new published synthesis runtime $version"
