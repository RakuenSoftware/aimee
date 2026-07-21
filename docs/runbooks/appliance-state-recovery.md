# Runbook: SmoothNAS/tierd appliance state recovery

This runbook restores the two pieces of live state a SmoothNAS/tierd
appliance running the `aimee-server` plugin can lose on its tier-bound
volumes: the agent config (`$AIMEE_HOME/agents.json`) and a workspace
repo's git metadata (`.../workspaces/<user>/<repo>/.git`).

Every state-changing step in this runbook is preceded by the prerequisites
below. Do not skip them; under incident pressure, the wrong appliance,
tier, repository, branch, or filesystem identity is exactly how a
recovery becomes data loss.

## 1. Prerequisites and Safety Checks

Complete every check below before touching `$AIMEE_HOME` or any
workspace repo. If any check fails, stop and resolve it before
continuing — do not work around a failed prerequisite.

### 1.1 Confirm the host

Confirm that the shell you are running commands from is attached to the
**affected SmoothNAS/tierd appliance** and not a peer, a maintenance
shell, or a workstation tunneled in by mistake.

- Verify hostname, tier label, and tier-bound mount points match the
  incident ticket for the affected appliance.
- Confirm you are not on a clone, a test rig, or a recovered image
  still in dry-run.

If the host does not match, **stop immediately** and reattach to the
correct appliance. Running the recovery on the wrong box will restore
state to a healthy system and leave the broken one untouched.

### 1.2 Resolve `$AIMEE_HOME` and confirm it is tier-bound

The agent config and the vault live under `$AIMEE_HOME`, which must
resolve to the tier-bound volume for this appliance — not to a scratch
disk, a tmpfs, or a path that disappears on reboot.

- Echo `$AIMEE_HOME` and confirm the value is set (not empty).
- Resolve the path to its canonical form (no trailing symlinks) and
  confirm it is mounted on the tier-bound filesystem for this appliance
  (`mount | grep <canonical>` or equivalent).
- Confirm `$AIMEE_HOME/agents.json`, `$AIMEE_HOME/.vault/`, and any
  `agents.json.bak-*` siblings all live on that same tier-bound mount.

If `$AIMEE_HOME` is unset, points at the wrong tier, or resolves off
the tier-bound volume, **stop immediately** and fix the environment.
Restoring state to the wrong path is the most common way to "succeed"
the recovery while leaving the appliance still broken.

### 1.3 Identify and verify the affected workspace repository

For each workspace repo that needs recovery, record and verify:

- **Repository path** under the tier-bound workspace root (typically
  `.../workspaces/<user>/<repo>/.git` or its parent).
- **Canonical HTTPS clone URL** — the URL the forge and the proposals
  trigger use as the single source of truth for `origin`. Do not guess,
  derive from a local `origin`, or copy from a stale note.
- **Default branch** — the branch the fresh clone must track. Confirm
  it against the forge (web UI or `git ls-remote --symref <url> HEAD`)
  rather than against the local repo you are about to replace.

If the canonical HTTPS clone URL or the default branch cannot be
confirmed against an authoritative source, **stop immediately**. Cloning
from a guessed URL or tracking a guessed branch silently rewires the
workspace to the wrong upstream and is unrecoverable without operator
intervention.

### 1.4 Run all commands as the owner of the files

Every command in this runbook that reads, replaces, or otherwise
touches state under `$AIMEE_HOME` or a workspace repo must run as the
account that **owns those files**. Running as root, as another service
account, or as the operator's personal user will silently change
ownership or permissions on the restored files and break the appliance
on next restart.

- Confirm your shell's effective UID/GID matches the owner of
  `$AIMEE_HOME/agents.json`, any `agents.json.bak-*` candidates, and
  the workspace repo's `.git/`.
- If they differ, switch to the owning account (e.g. `sudo -u <owner>`
  or the equivalent on this tier) before any further command. Do not
  `chown` the files as a shortcut — that is itself a state change.

If no commands can be run as the owning account, **stop immediately**
and escalate. Permission drift on the agent config or vault directory
is a security-relevant incident, not a recovery problem to paper over.

### 1.5 Inspect and record current ownership and permissions

Before any replacement or restoration, snapshot the current ownership
and permissions of every file or directory that will be touched. This
snapshot is the rollback target if the recovery must be reverted.

- Record owner, group, and mode for `$AIMEE_HOME/agents.json` (if
  present), every `agents.json.bak-*` candidate, the parent directory
  of `$AIMEE_HOME/agents.json`, and the workspace repo's `.git/` and
  its parent.
- Use a non-mutating form (`stat`, `ls -ln`, `getfacl`) so the
  inspection itself does not update mtimes or atimes that downstream
  steps rely on.
- Store the snapshot alongside the incident notes; do not edit it
  during the recovery.

If any of these paths cannot be inspected (missing, permission denied,
on a non-tier mount), **stop immediately** and resolve access. You
cannot restore ownership and permissions after the fact if you did not
record them first.

### 1.6 Abort conditions — stop immediately if any of these are true

The following conditions mean the recovery as written cannot proceed
safely. Stop, do not improvise, and escalate.

- **No suitable `agents.json.bak-*` backup exists.** If the directory
  contains no `agents.json.bak-*` file (or none whose mtime predates
  the incident and whose owner matches the owning account), the
  backup-restore path for "lost/absent `agents.json`" is not available.
  Restoring from a corrupted or post-incident backup is worse than
  none; do not synthesize a new `agents.json` from memory.
- **Selected backup is not valid JSON or is missing expected agent
  names.** Before any restore, validate the candidate backup with a
  JSON parser and confirm it contains the agent names the incident
  ticket says should be present. If validation fails, the backup is
  not the one you think it is; pick a different candidate or stop.
- **Fresh clone on the same volume fails.** The workspace-recovery
  step depends on a healthy clone of the repo onto the same tier-bound
  volume. If `git clone <canonical-url> <scratch-on-tier>` errors out,
  the volume or the network is the problem, not the workspace.
  Replacing the `.git` directory on a broken volume will not fix it
  and may destroy forensic state.
- **Canonical HTTPS clone URL or default branch cannot be confirmed.**
  See §1.3. If either is unknown, the replacement clone will attach
  the workspace to the wrong upstream and the recovery will silently
  fail on the next proposals-trigger poll.

### 1.7 Handling secrets during validation

The vault (`$AIMEE_HOME/.vault/`) and the agent config reference API
keys and other secrets. During prerequisite validation it is common to
dump file contents, run `cat`, or echo decoded fields to confirm
structure.

- **Do not print, log, or otherwise expose API keys or other secrets**
  in verification output, in incident chat, in ticket comments, or in
  shell history.
- When validating `agents.json`, restrict output to structure (key
  names, counts, agent names) and avoid echoing values that look like
  credentials.
- When validating the vault directory, list files and permissions only;
  do not `cat` the master key or any encrypted secret file.
- Use a shell with history disabled, or prefix sensitive inspection
  commands with a leading space per local convention, so that commands
  are not retained in `~/.bash_history`.

Treat any secret that does appear in output as compromised: rotate it
following `docs/runbooks/vault-master-key-rotation.md` rather than
attempting to scrub the output.

---

Once every check in §1 has passed and recorded, proceed to the
failure-mode-specific recovery steps (§2 covers lost/absent
`agents.json` and stale-but-present `agents.json`; §3 covers a corrupt
or lost workspace repo git directory).
