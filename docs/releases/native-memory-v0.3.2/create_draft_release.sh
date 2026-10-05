#!/usr/bin/env bash
# Consume signed opaque artifacts; this repository does not build the plugins.
set -euo pipefail
if [ "$#" -ne 2 ]; then
  echo 'usage: create_draft_release.sh ASSET_DIRECTORY REVIEWED_AIMEE_COMMIT' >&2
  exit 2
fi
assets="$(realpath "$1")"
commit="$2"
here="$(cd -- "$(dirname -- "$0")" && pwd)"
if ! [[ "$commit" =~ ^[0-9a-f]{40}$ ]]; then
  echo 'expected an exact reviewed Aimee commit SHA' >&2
  exit 2
fi
python3 "$here/verify_assets.py" "$assets"
command -v gh >/dev/null || { echo 'GitHub CLI with release-write access is required' >&2; exit 2; }
# A read/authentication failure must stop, rather than imply the tag is free.
existing="$(gh api --paginate repos/RakuenSoftware/aimee/releases --jq '.[].tag_name')"
if [[ $'\n'"$existing"$'\n' == *$'\nnative-memory-v0.3.2\n'* ]]; then
  echo 'native-memory-v0.3.2 already exists; inspect it before proceeding' >&2
  exit 1
fi
files=(
  aimee_native_runtime-0.3.2-2-cp312-cp312-linux_x86_64.whl
  aimee_gemma4_12b-0.3.2-2-cp312-cp312-linux_x86_64.whl
  aimee_gemma4_26b-0.3.2-2-cp312-cp312-linux_x86_64.whl
  aimee_qwen3_8_27b-0.3.2-2-cp312-cp312-linux_x86_64.whl
  vllm_gguf_plugin-0.0.5+triton-py3-none-any.whl
  manifest.json SHA256SUMS
)
attachments=()
for name in "${files[@]}"; do
  attachments+=("$assets/$name" "$assets/$name.sig")
done
gh release create native-memory-v0.3.2 --repo RakuenSoftware/aimee \
  --target "$commit" --title 'Aimee native memory 0.3.2 (three-model preview)' \
  --notes-file "$here/RELEASE_NOTES.md" --draft --prerelease --latest=false \
  "${attachments[@]}"
echo 'Draft created. Review the server prerequisite and assets before publication.'
