# Aimee 1.1.4

A patch release in the `1.1` series. It fixes the delegate failure and the logging problems found
while validating 1.1.3 on the live Server.

## Upgrade

- No Compose or configuration change. Take the new images as for any patch release.
- No data migration. Stored memory, the audit ledger, Vault identity and sessions are kept.

## Fixes

- **A rejected memory call no longer ends a delegate.** 1.1.3 accepted only the host's fixed "no
  receipt" reply in a delegate's history. Any other error from a memory tool, such as a
  `memory_serve` call the model sent without its required `view` argument, was still refused and
  the delegate died with "failed to build request". An error result carries no memory, so it is
  now accepted and the delegate sees an ordinary tool error. Memory output without a receipt is
  still refused.
- **Read-only delegates clean up their worktree.** A read-only delegate's worktree starts with the
  client's uncommitted files, so it was always "dirty". The Server logged that the delegate had
  produced changes and that they were being discarded, but kept the worktree on disk. The worktree
  is now removed (its branch is kept), and the log no longer blames the delegate.
- **Hook identity after a Server restart.** Sessions in a Git repository stayed untrusted after a
  restart because the thin client only renewed its hook token outside Git repositories. It now
  renews in both. The "untrusted hook" warning is logged once per session and client.

## Logging

- Expected outcomes are no longer logged as failures: runner long-poll timeouts, a reply that
  needs a larger read buffer before it is retried, and `git merge-base --is-ancestor` answering
  "no".
- A memory refusal caused by how the deployment is configured, such as a missing documentary
  grade source adapter, warns the first time and logs at debug after that, instead of on every
  delegate start.
- A reply too large for the read that requested it is logged with its size.

## Unchanged

- Compose files, synthesis images and embedder images are the same as 1.1.3.
- Server images remain `linux/amd64` only; thin clients are built for every platform.
