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

approval_job=$(awk '
    /^  approval:[[:space:]]*$/ { in_approval = 1; print; next }
    in_approval && /^  [A-Za-z0-9_-]+:[[:space:]]*$/ { exit }
    in_approval { print }
' "$main_approval")

printf '%s\n' "$approval_job" |
    grep -Eq '^    environment:[[:space:]]+main-merge-approval[[:space:]]*$' ||
    fail "$main_approval must deploy through the protected main-merge-approval environment"

echo "release-policy: approval topology is intact"
