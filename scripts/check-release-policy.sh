#!/usr/bin/env bash
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)

fail() {
    echo "release-policy: $*" >&2
    exit 1
}

on_block() {
    awk '
        /^on:[[:space:]]*$/ { in_on = 1; next }
        in_on && /^[A-Za-z0-9_-]+:/ { exit }
        in_on { print }
    ' "$1"
}

require_reusable_only() {
    local file=$1
    local block
    block=$(on_block "$file")

    printf '%s\n' "$block" | grep -Eq '^  workflow_call:' ||
        fail "$file must expose workflow_call"

    if printf '%s\n' "$block" |
        awk '/^  [A-Za-z0-9_-]+:/ { key=$1; sub(/:$/, "", key); if (key != "workflow_call") exit 1 }'; then
        :
    else
        fail "$file has a direct event trigger; release workflows must be reusable-only"
    fi
}

job_block() {
    local file=$1
    local job=$2
    awk -v header="  ${job}:" '
        $0 == header { in_job = 1; print; next }
        in_job && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
        in_job { print }
    ' "$file"
}

require_job_permission() {
    local file=$1
    local job=$2
    local permission=$3
    local value=$4
    local block
    block=$(job_block "$file" "$job")

    [ -n "$block" ] || fail "$file has no $job job"
    printf '%s\n' "$block" |
        grep -Eq "^      ${permission}:[[:space:]]+${value}[[:space:]]*$" ||
        fail "$file $job job must grant ${permission}: ${value} to its reusable workflow"
}

auto_release="$repo_root/.github/workflows/auto-release.yml"
main_approval="$repo_root/.github/workflows/main-merge-approval.yml"
publish_images="$repo_root/.github/workflows/publish-images.yml"
release_client="$repo_root/.github/workflows/release-thin-client.yml"

require_reusable_only "$publish_images"
require_reusable_only "$release_client"

tag_job=$(awk '
    /^  tag:[[:space:]]*$/ { in_tag = 1; print; next }
    in_tag && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
    in_tag { print }
' "$auto_release")

printf '%s\n' "$tag_job" | grep -Eq '^    environment:[[:space:]]+release[[:space:]]*$' ||
    fail "$auto_release tag job must use the protected release environment"

grep -Fq 'uses: ./.github/workflows/publish-images.yml' "$auto_release" ||
    fail "$auto_release must call the guarded image publisher"
grep -Fq 'uses: ./.github/workflows/release-thin-client.yml' "$auto_release" ||
    fail "$auto_release must call the guarded thin-client publisher"

# Both publishers create keyless signatures and request an OIDC token. GitHub
# validates reusable-workflow permissions only when auto-release is triggered;
# without these caller grants the release fails at startup before any job exists.
require_job_permission "$auto_release" thin-clients id-token write
require_job_permission "$auto_release" images id-token write

for image_job in llm-images images; do
    image_job_block=$(job_block "$auto_release" "$image_job")
    printf '%s\n' "$image_job_block" |
        grep -Fq 'needs: [version, tag, thin-clients]' ||
        fail "$auto_release $image_job must wait for thin-client publication"
done

# The release artifact build must retain the platform crypto setup exercised by
# CI. macOS needs a universal libcrypto archive for its arm64+x86_64 binary;
# Windows needs the OpenSSL package matching the selected MSYS2 toolchain and
# must expose that prefix to CMake. Losing any of these makes the gated release
# fail only after it has created the version tag.
grep -Fq 'Build universal OpenSSL crypto (macOS)' "$release_client" ||
    fail "$release_client must build universal OpenSSL crypto for macOS"
grep -Fq 'lipo -create \' "$release_client" ||
    fail "$release_client must combine both macOS libcrypto architectures"
grep -Fq -- '-DOPENSSL_ROOT_DIR="$OPENSSL_ROOT_DIR"' "$release_client" ||
    fail "$release_client must pass its universal OpenSSL prefix to macOS CMake"
grep -Fq 'mingw-w64-ucrt-x86_64-openssl' "$release_client" ||
    fail "$release_client must install OpenSSL for the Windows UCRT toolchain"
grep -Fq 'mingw-w64-x86_64-openssl' "$release_client" ||
    fail "$release_client must install OpenSSL for the Windows MinGW toolchain"
grep -Fq 'echo "OPENSSL_ROOT_DIR=$($prefix -replace' "$release_client" ||
    fail "$release_client must pass its MinGW OpenSSL prefix to Windows CMake"

approval_job=$(awk '
    /^  approval:[[:space:]]*$/ { in_approval = 1; print; next }
    in_approval && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
    in_approval { print }
' "$main_approval")

printf '%s\n' "$approval_job" |
    grep -Eq '^    environment:[[:space:]]+main-merge-approval[[:space:]]*$' ||
    fail "$main_approval must deploy through the protected main-merge-approval environment"

echo "release-policy: approval topology is intact"
