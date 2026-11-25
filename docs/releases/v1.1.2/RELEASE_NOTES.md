# Aimee 1.1.2

A patch release in the `1.1` series. It fixes delegation failures found running 1.1.1 with a
MiniMax model: the provider answered every request, but delegates from a client working in its
session worktree could not get a workspace, and failed jobs could not be read from a remote CLI.

## Upgrade

- No Compose or configuration change. Take the new images as for any patch release.
- No data migration. Stored memory, the audit ledger, Vault identity and sessions are kept.

## Fixes

- **Delegation from a session worktree.** A client working inside its aimee session worktree is
  reconstructed on the Server with that session's branch checked out. Every hook and git operation
  for the session then tried to create the session worktree inside that checkout, which Git
  refuses, and recorded a path that did not exist as the session's worktree. A checkout already on
  the session's branch is now the session's tree; nothing is nested inside it and cleanup never
  removes it. A worktree failure now logs Git's actual reason.
- **Reading failed jobs.** `aimee jobs show`, `aimee jobs logs` and `delegate_status` answered
  "rpc response exceeds the 262144-byte relay limit" on a remote CLI for long jobs, because the
  reply carried the full transcript twice. Replies now fit: long text keeps its beginning and end
  with the original size reported, and the transcript is left out, with its length reported, when
  it does not fit. The failure reason is always kept whole.
- **Token statistics on PostgreSQL.** Per-role delegate metrics, per-agent stats and the
  costliest-sessions insight failed with "the store sent a number where an integer was expected".
  Their token sums now reach the bus as integers.
- **Model probe.** A configured model that differs from the provider's catalog only in case (for
  example `minimax-m3` against `MiniMax-M3`) is reported available, with the catalog's spelling
  as `model_listed_as`.
- **`--acp`.** `aimee delegate --acp <command>` names a program on the client, which a Server
  never runs. It is now refused at once, pointing to `--via <model>`, instead of creating a job
  that failed later with "canonical native execution owner unavailable for configured adapter".

## Unchanged

- Compose files, synthesis images and embedder images are the same as 1.1.1.
- Server images remain `linux/amd64` only; thin clients are built for every platform.
