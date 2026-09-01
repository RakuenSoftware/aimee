#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
output_file=${1:-${GITHUB_OUTPUT:-}}

if [ -z "$output_file" ]; then
    echo "next-release-version: output file argument or GITHUB_OUTPUT is required" >&2
    exit 2
fi

# The source tree declares MAJOR.MINOR. Tags determine the next patch in that
# series. Both the real release and the testing -> main validation call this
# script so the version embedded by validation cannot drift from the version
# an approved release will build.
series=$(awk '$1 == "#define" && $2 == "AIMEE_VERSION_SERIES" {gsub(/"/, "", $3); print $3; exit}' \
    "$repo_root/src/headers/aimee_version.h")
if ! [[ "$series" =~ ^[0-9]+\.[0-9]+$ ]]; then
    echo "::error::AIMEE_VERSION_SERIES must be MAJOR.MINOR; got '${series:-<empty>}'" >&2
    exit 1
fi

latest=$(git -C "$repo_root" tag --list "v${series}.*" --sort=-v:refname | head -n1)
if [ -z "$latest" ]; then
    next="${series}.0"
else
    patch=${latest##*.}
    if ! [[ "$patch" =~ ^[0-9]+$ ]]; then
        echo "::error::cannot read a patch number from tag $latest" >&2
        exit 1
    fi
    next="${series}.$((patch + 1))"
fi

if git -C "$repo_root" rev-parse -q --verify "refs/tags/v${next}" >/dev/null; then
    echo "::error::tag v${next} already exists; aborting to avoid clobbering a release" >&2
    exit 1
fi

{
    echo "version=$next"
    echo "series=$series"
    echo "latest=$latest"
} >> "$output_file"

echo "Next version: v${next} (series ${series}, highest in series ${latest:-<none>})"
