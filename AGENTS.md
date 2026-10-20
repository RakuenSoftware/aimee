# Repository operations

Use Aimee's registered Git/GitHub commands and existing authentication for
repository operations. Personally authenticated Git over SSH or `gh` are
fallbacks when Aimee is unavailable. Never mutate GitHub through an assistant
connector. Keep automatic Codex reviews enabled. Do not add assistant branding
or co-author trailers to commits or PR descriptions.

## Absolute attribution restriction

The attribution restriction has no exceptions. Do not merge commits or PRs
carrying co-authorship trailers, assistant identities, generated-by footers,
session backlinks, or GitHub-app opening attribution.

A blocked PR is blocked. Do not work around its rejection with a replacement
PR, fresh branch, empty commit, rerun, retarget, indirect merge, squash, rebase,
allowlist, historical exemption, skipped check, admin override, or disabled
review. A request to finish or merge work does not authorize such a bypass.
Report the concrete rejection and repair it without concealing its provenance.
An immutable app badge cannot be removed by editing the PR body.

Preserve fail-closed enforcement in CI and the merge executor. Every incoming
commit and its PR history must pass; unavailable or incomplete evidence is not
a pass. Never weaken this policy to make a requested merge succeed.
