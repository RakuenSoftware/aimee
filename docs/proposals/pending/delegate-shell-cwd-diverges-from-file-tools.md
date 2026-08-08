# A delegate's shell and its file tools resolve different roots

- **State:** PENDING — measured on a live appliance 2026-08-08; no fix implemented.

## Problem

A background `code` delegate can read and write repository files but cannot run a
single shell command, so it can change code and has no way to check the change.
The two halves of the same delegate resolve the same session worktree against
**different roots**.

Measured on the validation appliance with a server-side checkout at
`/var/lib/aimee-workspaces/aimee`:

- **File tools resolve against the repository.** `read_file`, `list_files` and
  `git_log` all succeeded, operating in the session worktree
  `/var/lib/aimee-workspaces/aimee/.aimee/worktrees/7c548df2-b127074c54171e76/main`.

- **Shell resolves against the ephemeral workspace.** Every `bash` and
  `execute_script` call was refused, with the composed command showing the root it
  had actually been given:

      error: guardrail blocked: cd /var/lib/aimee/delegate-ws/deleg-492-1786193373307793551-17/.aimee/worktrees/7c548df2-b127074c54171e76/main && pwd

  Same worktree id, different root: the *ephemeral* workspace prefix with the
  session-worktree suffix appended. That path does not exist, so the guardrail
  refuses it — correctly. Even `pwd` and `echo hello` fail this way.

## What is established, and what is not

**Established by measurement.** The two observations above, plus: the ephemeral
workspace is created at `src/server/server_compute.c:1473`, which calls
`run_cmd_set_cwd(ephemeral_ws)`, and `run_cmd()` prefixes every shell line with
`cd <tl_run_cwd> && …` (`src/util.c:715`). The file tools do not go through that
path, which is why only one half moves. So the shell's root is whatever
`tl_run_cwd` holds, and it held the ephemeral workspace with a session-worktree
suffix appended.

**Not established: which code appends that suffix.** A first reading blamed the
prefix-replacement at `src/server/server.c:713`, which maps a cwd under `git_root`
onto `worktree_path + suffix`. That is wrong, and the tree already proves it:
`worktree_for_cwd()` returns NULL when the cwd is *already* inside a worktree, and
`test_worktree_for_cwd` in `src/tests/test_guardrails.c:403` asserts exactly that
("CWD inside worktree should NOT match"). The `if (wt)` guard above that mapping is
therefore false in this case and the mapping never runs. Whoever picks this up
should start from `tl_run_cwd` and work backwards, not from that site.

That site already records the underlying gap:

> The ephemeral workspace does NOT contain the client's repo (it drops an
> AIMEE_WORKSPACE_NOTE.txt saying so); a background *code* delegate that must edit
> the client tree needs it provisioned server-side (follow-up).

Provisioning the repo server-side is necessary but **not sufficient**: with a real
checkout present and passed as `cwd`, the file tools found it and the shell still
did not.

## Why this is worse than a delegate that cannot run

A delegate with no shell is not merely limited, it is unable to verify itself, and
it does not know that. Asked to add one comment line to a 2157-line Go file, a
`code` delegate **truncated the file to 5 lines** — 2152 deletions — and could not
compile, test, or `gofmt` to notice. Writes are permitted; verification is not.

That combination should not be reachable. Either a delegate can check its own work
or it should not be able to write.

## Proposal

1. **One root per delegate turn.** The shell and the file tools must resolve the
   same session worktree. Whichever root the file tools bind, `run_cmd_set_cwd()`
   receives that same root — not the ephemeral workspace with the worktree suffix
   pasted on.

2. **Refuse the incoherent combination.** When a delegate is write-capable but its
   shell cannot execute in its own worktree, fail the dispatch with that reason
   instead of running it. An unverifiable write delegate is a configuration error,
   and it currently presents as a successful edit.

3. **Say which root was used.** The refusal above is readable only because it
   happened to echo the composed `cd`. The delegate's bound root, and whether it is
   the repository or an ephemeral workspace, belong in the turn's diagnostics.

## Deliberately not proposed

**Provisioning policy for server-side checkouts.** Which repository a remote
delegate should get, and who clones it, is an operator decision. This proposal is
only that the two halves of one delegate must agree on where they are.

## Acceptance criteria

- A background `code` delegate given a server-visible checkout runs `pwd` and
  `go test` in the same worktree its file tools write to.
- A delegate whose shell cannot execute in its bound worktree is refused before it
  can write, with a diagnostic naming the root it was given.
- A test covers the divergence directly: file-tool root and shell root for one
  delegate turn are asserted equal.
- The turn diagnostic names the bound root and whether it is a repository checkout
  or an ephemeral workspace.

## Verification available to whoever takes this

The C suite builds and runs on an ordinary workstation (gcc 14, GNU make) — a
single test binary builds with

    make -C src build/obj/tests/unit-test-delegate-ephemeral-ws

so the path-composition half of this is unit-testable without a server or a model
provider. What a workstation cannot exercise is a real delegate turn end to end,
which needs a running server and a model backend; that is the part to reproduce on
an appliance.

## Evidence

Delegate jobs 36 (read-only probe) and 38 (write probe) against
`aimee-server:testing` in container `aimee-aimee-server-1`, appliance
`192.168.1.210`, 2026-08-08. Job 36 recorded the working file tools and the blocked
shell with the verbatim path above; job 38 produced the 2152-line deletion. The
truncated file was restored and nothing was committed or pushed.

`worktrees.tsv` on that appliance carried two rows for the same session key
`7c548df2-b127074c54171e76` — one under
`/var/lib/aimee-workspaces/environment/rakuensoftware/aimee` and one under
`/var/lib/aimee-workspaces/aimee` — so a session key can be registered against more
than one git root at once. Whether that plurality contributes to the wrong root is
untested and is a lead, not a conclusion.
