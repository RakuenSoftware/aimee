#!/bin/sh
set -eu

out=${1:?usage: generate-assurance-evidence.sh OUTPUT_DIRECTORY}
mkdir -p "$out"
git rev-parse HEAD > "$out/source-commit.txt"
git status --porcelain=v1 > "$out/source-status.txt"
cp docs/security-claims.json "$out/security-claims.json"
cp docs/compliance/CONTROL_EVIDENCE.md "$out/control-evidence-register.md"
find .github/workflows -maxdepth 1 -type f -name '*.yml' -print0 \
  | sort -z | xargs -0 sha256sum > "$out/workflow-sha256.txt"
find . -name go.sum -o -name package-lock.json -o -name Cargo.lock \
  | sort | xargs sha256sum > "$out/dependency-lock-sha256.txt"
python3 scripts/check-security-claims.py > "$out/security-claims-check.txt"
python3 scripts/check-workflow-pins.py > "$out/workflow-pin-check.txt"
sha256sum "$out"/* > "$out/MANIFEST.sha256"
if [ "${ASSURANCE_LIVE_GOVERNANCE:-0}" = 1 ]; then
  scripts/collect-live-governance-evidence.sh "$out/live-governance" "${GITHUB_REPOSITORY:-}"
fi
echo "assurance evidence written to $out"
