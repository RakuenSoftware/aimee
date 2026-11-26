# Aimee 1.1.3

A patch release in the `1.1` series. It fixes the remaining delegation failures found on the live
1.1.2 Server: delegates that used memory died, long delegate runs failed after finishing, and
`memory_serve` never worked inside a delegate.

## Upgrade

- No Compose or configuration change. Take the new images as for any patch release.
- No data migration. Stored memory, the audit ledger, Vault identity and sessions are kept.

## Fixes

- **Long delegate runs.** A run that finished its work could still fail with "canonical run
  checkpoint failed": saving its history had the ordinary 2-second budget, and a long transcript
  took longer. History saves now get the 30-second history budget on both the client and the store.
- **One failed memory call no longer ends a delegate.** When a memory tool could not return a
  receipted result, the next request refused the whole history and the delegate died with "failed
  to build request". The host's fixed refusal carries no memory, so it is now accepted and the
  delegate sees an ordinary tool error.
- **`memory_serve` in delegates.** Every call failed: the host added the delegate's current
  project to the call, and personal memory has no project and refused it. Personal memory reads no
  longer carry the ambient project; shared reads and a project the model asks for are unchanged.
- **Delegate memory context.** The memory context assembled before each delegate run was sent
  without the caller's identity and was always refused. It now carries it, as ordinary sessions do.
- **Diagnosable memory failures.** `memory.runtime` calls log their elapsed time against their
  budget, the memory owner's error and the identity and authority they were made with, so a refusal
  is no longer a bare "invalid request". Request building names the step that refused.

## Unchanged

- Compose files, synthesis images and embedder images are the same as 1.1.2.
- Server images remain `linux/amd64` only; thin clients are built for every platform.
