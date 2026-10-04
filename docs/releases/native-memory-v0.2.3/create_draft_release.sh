#!/usr/bin/env bash
# After PR review, attach the signed executable payload to the Aimee release.
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "usage: create_draft_release.sh ASSET_DIRECTORY REVIEWED_AIMEE_COMMIT" >&2
  exit 2
fi
assets="$(realpath "$1")"
commit="$2"
here="$(cd -- "$(dirname -- "$0")" && pwd)"
python3 "$here/verify_assets.py" "$assets"
if ! [[ "$commit" =~ ^[0-9a-f]{40}$ ]]; then
  echo "expected an exact reviewed Aimee commit SHA" >&2
  exit 2
fi
command -v gh >/dev/null || { echo "GitHub CLI with release-write access is required" >&2; exit 2; }
if gh release view native-memory-v0.2.3 --repo RakuenSoftware/aimee >/dev/null 2>&1; then
  echo "native-memory-v0.2.3 already exists; inspect it before proceeding" >&2
  exit 1
fi

gh release create native-memory-v0.2.3 \
  --repo RakuenSoftware/aimee \
  --target "$commit" \
  --title 'Aimee native memory v0.2.3 (self-hosted preview)' \
  --notes-file "$here/RELEASE_NOTES.md" \
  --draft --prerelease --latest=false \
  "$assets/aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl" \
  "$assets/aimee_vllm-0.2.3-1-cp312-cp312-linux_x86_64.whl.sig" \
  "$assets/vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl" \
  "$assets/vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl.sig" \
  "$assets/manifest.json" "$assets/manifest.json.sig" \
  "$assets/SHA256SUMS" "$assets/SHA256SUMS.sig"

echo 'Draft created. Check its assets, then publish and verify a clean public download.'
