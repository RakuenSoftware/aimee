# Troubleshooting

For server/KB liveness, retrieval readiness, breaker diagnostics, queue depth, and
safe dependency recovery, see [Retrieval readiness and recovery](runbooks/retrieval-readiness-and-recovery.md).

Start at the first broken boundary.

```bash
aimee remote status
aimee status
aimee kb status
aimee audit verify
```

## Client cannot connect

Check URL, DNS, port, server health, certificate fingerprint, bearer rotation, client certificate,
and revocation. A changed pin is a stop, not a warning to bypass.

For local use, check the Unix socket, service manager, server log, and config-directory ownership.

## Reads work; writes fail

Read the structured `403` first. For the first wizard user, confirm the Linux client completed mTLS
enrollment with the exact command shown after the summary's Deploy action; using Deploy again as that same user is
idempotent and shows the pairing state. For an additional PAM/OIDC user, check `AIMEE_SERVER_ID`,
`AIMEE_SERVER_TEAM_ID`, the management-JWKS trust bundle, exact subject spelling, grant tier, and
identity-token refusal reason. `aimee.api.remote_writes` cannot fix either denial. Do not widen every
user to test one grant.

## Session start refuses to create a worktree

The message names the repository and says the session worktree base could not be resolved. A new
session's branch is cut from the repository's default branch, and aimee will not fall back to the
branch the shared checkout has checked out, because that put new sessions on another session's work.

Set the remote's default with `git remote set-head origin -a`. Without a remote, set
`session_worktree_base` or `AIMEE_SESSION_WORKTREE_BASE` to an explicit ref. `current` is accepted
but only as a deliberate choice for offline work; it inherits the checked-out branch.

Until this resolves, the session has no worktree and the isolation guard refuses its writes.

## A worktree under an old key was kept

The message reads `kept pre-rekey worktree <path>`. Session worktree keys changed to stop two
sessions sharing one checkout, so a session that predates the change owns a worktree under the old
key. Reclaim removes the old one only when it is clean.

That worktree holds uncommitted or unpushed work. Recover it, then remove the worktree:

```bash
git -C <path> status
git -C <path> worktree remove <path>
```

## KB is unavailable

Check `AIMEE_KB_API_URL`, service bearer, TLS, KB health, PostgreSQL readiness, extensions, disk, and
connection limits. The server does not autostart a missing KB.

## Search returns nothing

Check scope, ingest status, document commit, code-index freshness, embedder readiness, and filters.
An honest degraded lexical result differs from an empty ingest.

## Delegate fails

Run `aimee agent probe <name>`. Then inspect admission, provider auth, model capability, agent limit,
workspace authority, worktree, sandbox image, package gate, network policy, and the first attempt log.

No network is the container default. Add the narrow egress the task needs; do not disable isolation
globally.

## Delegate cannot open a file that is plainly there

The file tool answers `cannot open <path>` for a file you can see in the delegate's worktree.

Its sandbox mounted an empty directory. When aimee-server runs in a container and drives a sibling
Docker daemon, a bind source expressed in the server's own filesystem does not exist on the daemon's
host, and Docker creates it empty rather than failing. Look at where the mount actually points:

```bash
docker inspect <aimee-delegate-...> --format '{{range .Mounts}}{{.Source}} -> {{.Destination}}{{println}}{{end}}'
```

A source equal to its destination on a sibling-daemon host means the translation did not apply. The
entrypoint derives it and says so at startup:

```bash
docker logs <server> 2>&1 | grep 'derived host-path map'
```

See [Delegate sandbox](DELEGATE_SANDBOX.md).

## Delegated shell is refused as unsandboxed

```text
refused: a delegated shell requires sandbox isolation, but the sandbox is off/unavailable
```

The delegate has no assigned worktree, so there was nothing to containerise and it fell through to
running beside the server. That refusal is correct: aimee-server holds the Docker socket, so an
unsandboxed shell there is a host-root escalation. Give the delegate a workspace. Do not reach for
`sandbox.mode` to make the message go away.

## A settings change seems to have been ignored

Read the log before assuming the field needs a restart. The C server writes to a file, not to the
container's stdout, so `docker logs` will not show this:

```bash
docker exec <server> grep 'config file change' /var/lib/aimee/server.log | tail -3
```

`rejected (kept running config)` means the file did not validate and the previous configuration is
still serving, so the edit did nothing. `reloaded` means it applied, and the field is live.

If neither line appears, the field is one of the startup-bound ones. [Settings](SETTINGS.md) lists
the classes.

## A deploy reported success and changed nothing

Check what is actually running before believing any other result:

```bash
docker pull <image>            # let the output show; a quiet pull hides a no-op
docker image inspect <image> --format '{{.Id}}'
docker inspect <container> --format '{{.Image}}'
```

A tag that looks current makes `pull` a no-op, Compose then reports `Running` and keeps the old
container, and every check after that silently tests the previous build. Assert the two IDs match
before drawing a conclusion. `aimee --version` reporting an older commit than your branch is normal
for a docs-only change, because the image only rebuilds when image-affecting paths change.

## `kb status` reports failed jobs and nothing says why

```text
Queue:     0 pending, 0 running, 9 failed
```

Ask the jobs, not the counter:

```bash
docker exec <kb> sh -c "/usr/lib/postgresql/18/bin/psql --host=/var/lib/aimee/run \
  --dbname=aimee_shared --no-psqlrc -c \"select kind, left(last_error,80), count(*) \
  from kb_async_jobs where status='failed' group by 1,2\""
```

On a default install the answer is usually one row:

```text
 memory_facts | no curator provider or command configured | 9
```

That is not a storage fault. The selected KB has no ready synthesis role, so curator work has nowhere
to run and each attempt is recorded as a failure rather than skipped. The count grows quietly and
`kb status` does not name the cause.

Deploy the model-specific `aimee-llm-e2b` or `aimee-llm-e4b` sidecar for that KB, or point the KB at
a supported remote endpoint. Otherwise expect the count. `aimee kb status` shows the curator tiers as
`configured: false` until a role is ready. See [KB model tiers](AIMEE_KB_SYNTH_TIERS.md).

## Workflow parks

The park reason names the constraint that stopped the scheduler. Read it with the current node and
last lifecycle events before changing a limit.

Common causes are a human gate, no valid panel, repeated feedback, agent saturation, missing commit,
failed verification, merge conflict, lost replay, forge identity, or spend limit.

Repair the condition and resume the same run so its evidence is preserved.

## Audit or capture gap

Check the event-bus drop counter, sink write errors, free disk, capture classification, and whether the
daemon shut down cleanly. `publish` success means accepted into the producer ring, not yet durable.
Graceful stop drains; a crash may leave the last capture classified open or truncated.

Do not rewrite or prune the WORM store while investigating. Seal a copy first.

## Browser deploy fails

Check the Docker socket mount, daemon access, image pulls, volume ownership, port conflicts, and the
managed server log. If Docker authority is intentionally absent, use the split stack.

## Report a useful failure

Include:

- version and commit;
- deployment shape and platform;
- operation/request ID;
- exact command and structured error;
- first relevant log error;
- health output with secrets removed;
- whether the failure survives restart;
- the smallest reproduction.

Never attach tokens, client keys, vault material, database URLs with passwords, or raw memory that
contains private data.
