# Prerequisites and Safety Checks — SmoothNAS/tierd appliance state recovery

This document is the **§1 Prerequisites and Safety Checks** section of the
SmoothNAS/tierd appliance state recovery runbook. It is published as a
standalone file so the prerequisites can be reviewed, signed off, and
referenced under incident pressure without dragging the full runbook into
the operator's working set. The failure-mode-specific recovery steps
(restoring a lost or absent `$AIMEE_HOME/agents.json`, and replacing a
corrupt or lost workspace repo `.git/`) live in the runbook proper and
must be executed from there once every check in this section has passed.

State-changing recovery or replacement steps can target the wrong
appliance, tier, repository, branch, or filesystem identity if the
prerequisites below are not verified first. The checks in this section
exist to reduce the risk of data loss, permission drift, invalid
restoration, failed same-volume replacement, and secret exposure. Do not
skip them.

## 1. Prerequisites and Safety Checks

Each check below is scoped: §1.1, §1.2, §1.3, §1.4, §1.5, and §1.7
apply to **every** recovery path (the `agents.json` restoration path
and the workspace `.git/` replacement path). The §1.6 abort conditions
are scoped per path: the backup-availability and backup-validation
conditions apply only to the `agents.json` restoration path, while
the fresh-clone and unconfirmed canonical-URL/default-branch
conditions apply only to the workspace `.git/` replacement path. Do
not apply a path-specific condition to the other path. If any check
fails, stop and resolve it before continuing — do not work around a
failed prerequisite.

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

Every command in the recovery runbook that reads, replaces, or otherwise
touches state under `$AIMEE_HOME` or a workspace repo must run as the
account that **owns those files**. Running as root, as another service
account, or as the operator's personal user will silently change
ownership or permissions on the restored files and break the appliance
on next restart.

The owner check applies to the **required-surviving** paths the recovery
expects to find in place: the parent directory of `$AIMEE_HOME/agents.json`,
`$AIMEE_HOME/.vault/`, every `agents.json.bak-*` candidate, and the
parent directory of the workspace repo's `.git/`. For the recovery
target itself, the rule is:

- `$AIMEE_HOME/agents.json` when it is being restored: may be absent
  by definition; capture its owning identity from the surviving parent
  directory and the matching `agents.json.bak-*` candidate.
- The workspace repo's `.git/` when it is being replaced: **inspect
  the existing `.git/` directory itself** — when it is present (even
  if corrupt), its UID/GID, mode, and ACLs are the authoritative
  baseline. A present `.git/` can have a different owner or mode than
  its parent workspace directory; do not derive owner from the parent
  in that case. Fall back to the parent directory's metadata only when
  `.git/` is genuinely absent.

- Confirm your shell's effective UID/GID matches the owner of every
  required-surviving path listed above **and**, for the workspace
  `.git/` replacement path, matches the owner recorded from the
  present `.git/` (or its parent when absent).
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

The snapshot is partitioned into **required-surviving paths** (which
must be inspectable now, on pain of stopping) and **expected-missing
recovery targets** (whose absence is the reason the recovery is being
run and must be recorded explicitly, with the baseline captured from
the surviving parent and any candidate backups):

- **Required-surviving — must be inspectable:**
  - The parent directory of `$AIMEE_HOME/agents.json`.
  - `$AIMEE_HOME/.vault/` and its contents.
  - Every `agents.json.bak-*` candidate and its parent directory.
  - The parent directory of the workspace repo's `.git/`.
- **Recovery-target handling:**
  - `$AIMEE_HOME/agents.json`: this file is the agent config; when the
    incident is a lost/absent `agents.json`, its absence is the
    trigger. Record the absence, then capture the ownership/permissions
    baseline from its parent directory and from the matching
    `agents.json.bak-*` candidate.
  - The workspace repo's `.git/`: this directory is being **replaced**
    because it is corrupt or lost. **If `.git/` is present, inspect
    and record its own UID/GID, mode, and ACLs as the baseline** —
    a present `.git/` is the object being replaced, not the parent
    directory, and may have a different owner or permissions than
    its parent workspace directory. **If `.git/` is genuinely
    absent** (e.g. deleted), record the absence and capture the
    owning UID/GID and mode from the parent workspace repo directory
    as the baseline.
- Use a non-mutating form (`stat`, `ls -ln`, `getfacl`) so the
  inspection itself does not update mtimes or atimes that downstream
  steps rely on.
- Store the snapshot alongside the incident notes; do not edit it
  during the recovery.

If any **required-surviving** path cannot be inspected (missing,
permission denied, on a non-tier mount), **stop immediately** and
resolve access. You cannot restore ownership and permissions after the
fact if you did not record them first. A missing or corrupt recovery
target is recorded, not a stop condition.

### 1.6 Abort conditions — stop immediately if any of these are true

These conditions are scoped to the recovery path that triggers them.
Stop, do not improvise, and escalate. Do not apply a path-specific
condition to the other recovery path.

**`agents.json` restoration path only:**

- **No suitable `agents.json.bak-*` backup exists.** If the directory
  contains no `agents.json.bak-*` file (or none whose mtime predates
  the incident and whose owner matches the owning account), the
  backup-restore path for "lost/absent `agents.json`" is not available.
  Restoring from a corrupted or post-incident backup is worse than
  none; do not synthesize a new `agents.json` from memory. **Stop.**
- **Selected backup is not valid JSON or is missing expected agent
  names.** Before any restore, validate the candidate backup with a
  JSON parser and confirm it contains the agent names the incident
  ticket says should be present. If validation fails for the
  selected candidate, that candidate is **stopped** and must not be
  used. Any alternative candidate must independently re-pass the
  full §1.4 owner check, the §1.5 ownership/permissions snapshot,
  the incident-time mtime check, the JSON validity check, and the
  expected-agent-names check before recovery resumes. A validation
  failure on one candidate is not a free pass to try another without
  revalidation.

**Workspace `.git/` replacement path only:**

- **Fresh clone on the same volume fails.** The workspace-recovery
  step depends on a healthy clone of the repo onto the same tier-bound
  volume. If `git clone <canonical-url> <scratch-on-tier>` errors out,
  the volume or the network is the problem, not the workspace.
  Replacing the `.git` directory on a broken volume will not fix it
  and may destroy forensic state. **Stop.**
- **Canonical HTTPS clone URL or default branch cannot be confirmed.**
  See §1.3. If either is unknown, the replacement clone will attach
  the workspace to the wrong upstream and the recovery will silently
  fail on the next proposals-trigger poll. **Stop.**

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
following [`docs/runbooks/vault-master-key-rotation.md`](vault-master-key-rotation.md)
rather than attempting to scrub the output.

---

Once every check in §1 has passed and recorded, return to the recovery
runbook proper and proceed to the failure-mode-specific steps there
(restoring a lost or absent `$AIMEE_HOME/agents.json`, or replacing a
corrupt or lost workspace repo `.git/`).
