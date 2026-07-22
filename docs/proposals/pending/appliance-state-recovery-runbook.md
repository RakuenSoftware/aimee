# Proposal: appliance state-recovery runbook

- **State:** pending — single slice, documentation only.
- **Author:** JBailes
- **Charter roles:** Recall (operator orientation), Constrain-Verify (a checklist a human follows under incident pressure).

## Thesis

A SmoothNAS/tierd appliance running the `aimee-server` plugin can lose two
pieces of live state on its tier-bound volumes: the agent config
(`$AIMEE_HOME/agents.json`) and a workspace repo's git metadata
(`.../workspaces/<user>/<repo>/.git`). When that happens the symptoms are
specific and the recovery is mechanical, but the steps are not written down
anywhere, so each incident is rediscovered from scratch.

## Task (documentation only)

Add a new runbook at `docs/runbooks/appliance-state-recovery.md` that an
operator can follow verbatim. It must cover:

1. **Lost/absent `agents.json`.** Symptom: `GET /v1/agents` returns 502
   "agents backend unavailable" (note: `GET /v1/agent/list` masks the failure
   as an empty array). Recovery: restore from a sibling `agents.json.bak-*`
   backup (API keys live in the vault keyed by agent name, so a restored file
   needs no secrets re-entered), then `touch $AIMEE_HOME/agents.json` to bust
   the identity (mtime+size+inode) config cache.
2. **Stale-but-present `agents.json`.** Symptom: config looks absent though the
   file is clearly valid — an mtime in the past vs the box clock. Fix: `touch`.
3. **Corrupt/lost workspace repo git dir.** Symptom: the proposals trigger logs
   `ls-tree failed ... rc=128` every poll; the forge logs `resolve https origin:
   no origin remote`. Recovery: confirm a fresh clone on the same volume is
   healthy (rules out storage), then replace the repo with a clean
   single-branch clone that sets `origin` to the canonical HTTPS URL and tracks
   the default branch.

Keep it concise and checklist-shaped. No code changes; documentation only.

## Acceptance

- `docs/runbooks/appliance-state-recovery.md` exists and covers the three
  failure modes above with concrete symptoms + recovery commands.
- No source files changed.
