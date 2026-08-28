#!/bin/sh
# Export the live GitHub controls source files cannot prove, then sign the
# content-addressed bundle. Read-only: every gh call is GET.
set -eu
umask 077

usage() {
  echo "usage: collect-live-governance-evidence.sh OUTPUT_DIRECTORY [OWNER/REPO]"
}
case "${1:-}" in
  -h|--help) usage; exit 0 ;;
  '') usage >&2; exit 2 ;;
esac

out=$1
repo=${2:-${GITHUB_REPOSITORY:-}}
if [ -z "$repo" ]; then
  repo=$(git remote get-url origin | sed -n 's#.*github.com[:/]\([^/][^/]*/[^/][^/]*\)\(.git\)\{0,1\}$#\1#p')
fi
case "$repo" in
  *[!A-Za-z0-9_.\/-]*|/*|*/|*//*|*/*/*|'') echo "invalid OWNER/REPO: $repo" >&2; exit 2 ;;
esac
: "${AIMEE_EVIDENCE_SIGNING_KEY:?set AIMEE_EVIDENCE_SIGNING_KEY to an offline PEM private key}"
command -v gh >/dev/null
command -v jq >/dev/null
command -v openssl >/dev/null

if [ -e "$out" ]; then
  echo "refusing to mix evidence into existing path: $out" >&2
  exit 2
fi
parent=$(dirname "$out")
mkdir -p "$parent"
stage=$(mktemp -d "$parent/.governance-evidence.XXXXXX")
trap 'rm -rf "$stage"' EXIT HUP INT TERM

gh api -H 'Accept: application/vnd.github+json' "/repos/$repo/rulesets" > "$stage/repository-rulesets.json"
gh api -H 'Accept: application/vnd.github+json' "/repos/$repo/branches/testing/protection" > "$stage/testing-branch-protection.json"
gh api --paginate -H 'Accept: application/vnd.github+json' "/repos/$repo/environments?per_page=100" \
  | jq -s '{total_count:(map(.environments|length)|add),environments:(map(.environments)|add)}' \
  > "$stage/environments.json"
gh api --paginate -H 'Accept: application/vnd.github+json' "/repos/$repo/collaborators?affiliation=all&per_page=100" \
  | jq -s 'add | [.[] | {login,permissions,role_name}]' > "$stage/collaborator-access.json"
gh api -H 'Accept: application/vnd.github+json' "/repos/$repo" \
  | jq '{full_name,default_branch,visibility,archived,disabled,pushed_at}' > "$stage/repository.json"

jq -e 'type=="array" and any(.[]; .enforcement=="active")' \
  "$stage/repository-rulesets.json" >/dev/null
jq -e '.required_pull_request_reviews as $r |
       ($r.required_approving_review_count >= 1) and
       ($r.require_code_owner_reviews == true) and
       ($r.require_last_push_approval == true)' \
  "$stage/testing-branch-protection.json" >/dev/null
jq -e '.full_name and .default_branch' "$stage/repository.json" >/dev/null

{
  printf 'schema=github-governance-evidence.v1\n'
  printf 'repository=%s\n' "$repo"
  printf 'collected_at=%s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  printf 'collector=%s\n' "$(gh api /user --jq .login)"
  printf 'source_commit=%s\n' "$(git rev-parse HEAD)"
} > "$stage/METADATA"
(cd "$stage" && sha256sum collaborator-access.json environments.json repository-rulesets.json \
  repository.json testing-branch-protection.json METADATA > MANIFEST.sha256)
openssl dgst -sha256 -sign "$AIMEE_EVIDENCE_SIGNING_KEY" -out "$stage/MANIFEST.sha256.sig" \
  "$stage/MANIFEST.sha256"
if [ -n "${AIMEE_EVIDENCE_SIGNING_CERT:-}" ]; then
  cp "$AIMEE_EVIDENCE_SIGNING_CERT" "$stage/signer.pem"
fi
mv "$stage" "$out"
trap - EXIT HUP INT TERM
echo "signed live governance evidence written to $out"
