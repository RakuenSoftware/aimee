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

## The provisioning half is mostly not a gap

`workspace_turn_bind_active` (`src/modules/workspace/workspace_turn.c`) already
has two server-side paths, and they differ in exactly the way that matters here:

| Provider | Bound by | Needs a live client? |
|---|---|---|
| `mirror` | `bind_mirror(cwd, root, remote, head)` — `remote`/`head` read from `config_workspace_vcs_remote()` / `config_workspace_vcs_head()`, then `mirror_reconstruct_cwd()` materialises a real worktree from the server's own bare mirror | **No** |
| `detached` | the runner queue, served by the client's `aimee workspace serve` loop | **Yes** |

So provisioning a server-side tree for a background delegate is a solved problem:
it is the `mirror` provider, and it works with no client present because the head
and remote are persisted in config rather than reported per turn.

A `detached` workspace is unserviceable by a background job **by design** — the
files live on the client, and the job runs after that client has gone. That is
not an omission to be filled in; it is what "detached" means. The refusal
therefore names the mirror route with its exact command rather than the vague
"use a different provider", and explains why detached cannot work in that
position.

What remains genuinely open is narrower than this document first claimed:
a detached workspace that *does* carry a `remote` and `head` could fall back to
the mirror tier instead of being refused. Whether that is desirable is a real
question — it would run the delegate against the last synced state rather than
the client's current tree, which is its own way to be quietly wrong — and it is
an operator-facing decision, not an obvious win.

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

## In-process status: the branch is not reached, cause unknown

Instrumenting the branch that creates the ephemeral workspace
(`src/server/server_compute.c`, the `detached_bound && background_job_id > 0`
test) and driving a background write delegate through the harness shows the
delegate **never reaches that line**: with markers bracketing the turn, no
instrumentation output appears between them.

    PROBE-BEGIN ws=[/tmp/aimee-probe-ewAJoT/repo]
    PROBE rc=0 submitted=1 err=[]
    PROBE-END

The delegate dispatches and its worker runs, so it returns earlier for a reason
not yet identified — possibly harness-specific (no real agent config, no provider)
rather than anything to do with the appliance behaviour. **No mechanism is claimed
here.**

### Retractions

Three causal claims were published in earlier drafts and are withdrawn. They are
listed rather than deleted so the next reader does not re-derive them:

1. **The prefix-replacement at `src/server/server.c:713`.** Withdrawn:
   `worktree_for_cwd()` returns NULL when the cwd is already inside a worktree, and
   `test_worktree_for_cwd` (`src/tests/test_guardrails.c:403`) asserts it, so the
   mapping never runs in this case.
2. **A "silent no-op" from `rc=0 submitted=0 response=(none)`.** Withdrawn: the
   probe was malformed. The delegate was refused with `prompt too short (19 chars)`,
   sitting unread in the harness's `g_last_error`.
3. **"`cwd` is empty inside the worker."** Withdrawn: the `cwd=[]` lines came from
   *other* tests in the same suite, not from the probed delegate, which never
   reached the instrumented line at all.

What survives all three is only what was measured on the appliance: the file tools
resolve the repository worktree and work, the shell is handed a path under the
ephemeral workspace and is refused, and a `code` delegate truncated a 2157-line
file with no way to notice.

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
