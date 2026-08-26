#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: $0 PROJECT_ROOT ED25519_PRIVATE_KEY OUTPUT_MANIFEST" >&2
  exit 2
fi
project_root=$(realpath "$1")
private_key=$(realpath "$2")
output=$3
skills_root="$project_root/.aimee/skills"
[[ -d "$skills_root" && -f "$private_key" ]] || {
  echo "project skills directory or signing key is missing" >&2
  exit 1
}

output_dir=$(dirname "$output")
install -d -m 0700 "$output_dir"
manifest_tmp=$(mktemp "$output_dir/.skill-approvals.XXXXXX")
signature_tmp=$(mktemp "$output_dir/.skill-approvals-signature.XXXXXX")
trap 'rm -f -- "$manifest_tmp" "$signature_tmp"' EXIT

while IFS= read -r -d '' path; do
  [[ -f "$path" && ! -L "$path" ]] || continue
  links=$(stat -c '%h' "$path")
  [[ "$links" == 1 ]] || {
    echo "refusing multiply-linked skill artifact: $path" >&2
    exit 1
  }
  digest=$(sha256sum "$path" | cut -d' ' -f1)
  printf '%s  %s\n' "$digest" "$(realpath "$path")"
done < <(find "$skills_root" -type f -print0 | sort -z) >"$manifest_tmp"
[[ -s "$manifest_tmp" ]] || {
  echo "no project skill artifacts found" >&2
  exit 1
}

openssl pkeyutl -sign -rawin -inkey "$private_key" -in "$manifest_tmp" -out "$signature_tmp"
[[ $(stat -c '%s' "$signature_tmp") == 64 ]] || {
  echo "signing key is not Ed25519" >&2
  exit 1
}
install -m 0444 "$manifest_tmp" "$output"
xxd -p -c 128 "$signature_tmp" | tr -d '\n' >"$output.sig"
printf '\n' >>"$output.sig"
chmod 0444 "$output.sig"

public_hex=$(openssl pkey -in "$private_key" -pubout -outform DER | tail -c 32 | xxd -p -c 64)
printf 'AIMEE_SKILL_APPROVAL_MANIFEST=%s\n' "$(realpath "$output")"
printf 'AIMEE_SKILL_APPROVAL_PUBLIC_KEY=%s\n' "$public_hex"
