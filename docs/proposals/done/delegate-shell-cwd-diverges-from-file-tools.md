# A delegate's shell and its file tools resolve different roots

- **State:** DONE — root-caused and fixed 2026-08-08; all four acceptance criteria met.

> **Delivered.** Shipped across six merged PRs. The headline defect turned out to
> have two independent causes, only one of which this proposal had identified:
> a guardrail verdict the tool dispatcher did not implement (#2429, #2431), and
> the root divergence described below (#2441). A delegate on a detached workspace
> also had no server-side tree to run in at all, which took chunked workspace
> sync (#2436) and recording a detached workspace's VCS coordinates (#2440) to
> resolve. See **Delivery** at the end.

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

The ephemeral workspace is created at `src/server/server_compute.c:1473`, which
calls `run_cmd_set_cwd(ephemeral_ws)`; `run_cmd()` then prefixes every shell line
with `cd <tl_run_cwd> && …` (`src/util.c:715`). The file tools do not go through
that path, which is why only one half moves.

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

## Evidence

Delegate jobs 36 (read-only probe) and 38 (write probe) against
`aimee-server:testing` in container `aimee-aimee-server-1`, appliance
`192.168.1.210`, 2026-08-08. Job 36 recorded the working file tools and the blocked
shell with the verbatim path above; job 38 produced the 2152-line deletion. The
truncated file was restored and nothing was committed or pushed.

## Delivery

### What the diagnosis got right, and what it missed

The proposal's measurement was accurate and its remedy was correct, but it named
one cause where there were two, and it did not notice that the shell was also
being refused for a reason that had nothing to do with roots.

**Cause 1 — an unimplemented verdict (not in this proposal).** `pre_tool_check`
answers 0=allow, 1=allow-with-path-rewrite, 2=block, 3=allow-with-command-rewrite.
`agent_tools_dispatch.c` implemented 0, 1 and 2, and treated **3 as a refusal**.
Every shell command a guardrail wanted to *rewrite* came back as
`guardrail blocked: …`, echoing the composed command — which is why the evidence
below reads as a path problem. It was, but the block on top of it was not.
Fixed in #2429, and again at a second call site in `server_compute_async.c`
(#2431), which had the same `rc != 0 && rc != 1` test.

That is worth stating plainly: **the verbatim path in the Problem section is
real, and the error message quoting it was produced by an unrelated bug.** Three
causal claims made while chasing it were wrong and were retracted rather than
quietly dropped (`server.c:713`; "silent no-op"; "cwd is empty") — each was an
inference from partial output, and each was corrected only after grepping the
appliance log for other occurrences of the same path.

**Cause 2 — the root divergence (this proposal).** Confirmed exactly as
described, and fixed in #2441.

### The fix

`delegate_worker` derives everything from `cwd`: the worktree the file tools
write to, the shell's working directory, the paths rewritten in the prompt, the
tree the no-op detector diffs. The detached-workspace reconstruction ran
hundreds of lines *below* all of that and moved only `run_cmd_set_cwd()`, so the
worktree had already been resolved against the client's path.

The resolution now happens **once, before anything derives from it**, and
rebinds `cwd` itself; the late redirect is deleted rather than kept in sync. The
two halves cannot come apart because there is only one value.

### Provisioning, which this proposal deliberately left out

"Deliberately not proposed" was right that provisioning policy is an operator
decision, but a delegate with coherent roots and no tree is still a delegate
that cannot work. Closing that took two changes the proposal did not anticipate:

- **#2436** — `workspace mirror-sync` ships its patch in 128 KB chunks. The
  binding cap was never the 4 MB transport limit but a **256 KB per-method**
  limit; chunking means no limit has to move, because the size of one request
  stops being a function of the size of the tree.
- **#2440** — a `detached` workspace can record a remote and head. The
  reconstruction path read them, but nothing could set them: `workspace add`
  stored them only for `mirror` and `mirror-sync` refused anything else, so that
  fallback was unreachable. This avoids making anyone trade live file serving
  for working delegates — a detached workspace stays detached.

### Acceptance criteria

| # | Criterion | Where |
|---|---|---|
| 1 | Shell and file tools resolve the same worktree | #2441 — root resolved once, above every consumer |
| 2 | Write-capable delegate with an unusable shell is refused before it can write | #2441 — refusal names the workspace and both ways out |
| 3 | A test asserts file-tool root == shell root for one turn | `test_bg_detached_mirror_delegate_roots_agree`, `test_delegate_shell_and_file_roots_agree` |
| 4 | The turn diagnostic names the bound root and its kind | `## Working root` block + an INFO line naming the root, its kind, and any divergence |

Criterion 3 was written first and observed to fail on unmodified code:

```
shell root [.../workspaces/56d321a8/work]
 != file write root [.../repo/.aimee/worktrees/bgdetached/main]
```

Both roots are sampled *during* the run; `delegate_worker` clears them on the way
out. Writing it first was what caught that the read-only detached case had never
actually run a delegate — the stub agent advertised only the `code` role, so a
`validate` delegate was rejected as unroutable before reaching any of the
behaviour under test.

### Verified

- **Live appliance** (`192.168.1.210`, container `aimee-aimee-server-1`): with a
  fixed `:testing` image, the guardrail block on `pwd` disappeared. The original
  refusal reproduced against the stock image first.
- **Chunked sync end to end** against a real server: 4,516,176 bytes in 36
  chunks, reassembled patch passes `git apply --check`, provider still `detached`.
- **Gates**: `make unit-tests`, `make lint` (48/48), `check_c_repository_lock`,
  `refactor_baselines`, `check_module_inventory`.

### Merged

| PR | |
|---|---|
| #2429 | guardrail verdict 3 in the tool dispatcher — the root cause |
| #2431 | the same verdict at a second call site |
| #2432 | oversized-body diagnostic said "cannot reach endpoint" for a plain refusal |
| #2436 | chunked `workspace mirror-sync` |
| #2440 | a detached workspace records what a client-less turn needs |
| #2441 | one root per delegate turn, and a diagnostic that names it |

### Not done

**Read-only delegates may still diverge.** A read-only delegate on an unsynced
detached workspace runs its shell in an ephemeral scratch dir while its file
tools point at the client workspace. Working in the wrong place is useless there
rather than destructive, so it is allowed — but it is now *declared*: the
delegate is told both roots and that it cannot verify anything. Making that case
coherent means provisioning a tree for it, which is the operator decision this
proposal set out of scope.
