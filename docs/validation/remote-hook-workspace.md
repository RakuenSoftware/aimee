# Remote hook workspace regression

Date: 2026-09-06. Related change: thin-client proxy, PR #2957.

## Failure and source repair

Regular Git reads worked in the client checkout, but the pre-tool hook rejected
pushes before Git executed: `cannot resolve target worktree`. Supplying an
explicit `git -C` path and tool `workdir` did not fix it.

Three source seams needed repair:

- `src/client_session_worktree.c` emits `cd -- '<worktree>' && ...`, but the
  target parser in `src/modules/guardrails/guardrails_orchestrator.c` treated
  the option terminator as the directory. The live diagnostic named the failed
  target as `--`. The parser now consumes that terminator before the path.

- `src/server/server.c`: the hook must bind the registered workspace provider
  before checking a tool, using the tool's effective working directory. A
  detached workspace's worktree routing remains client-owned. Provider and
  execution-directory state are cleared/restored after the check.
- `src/modules/guardrails/guardrails_orchestrator.c`: legacy worktree, branch,
  and verification helpers call `run_cmd`. For a detached workspace those
  probes must use its runner, not the server filesystem. A scoped, thread-local
  execution adapter covers the whole check, including ordinary detached tool
  calls, and restores the previous adapter on every returned verdict. Transport
  failure does not fall back to local command execution.

The verification, main-branch, and merged-PR policies are not disabled. A
detached workspace still requires its authorized client runner to be serving;
this change does not silently replace it with a stale server-side mirror.

## Regression coverage

- `src/tests/test_server_dispatch.c`: 30 combinations of Claude `Bash`, Codex
  `exec_command` / `functions.exec_command`, four working-directory keys plus
  outer-cwd fallback, object/string input encoding, and allow/deny verdicts.
  Checks provider binding, directory selection, and cleanup between requests.
- `src/tests/test_guardrails.c`: actual guardrails against a client-only
  checkout; successful resolution, enforced verification denial, main-branch
  denial, and unavailable-runner denial for all three shell-tool spellings,
  both raw commands and the launcher's `cd --` rewrite. The new rewrite case
  reproduced the live `root=--` failure before the parser fix and passed after it.
- `src/tests/test_util.c`: execution adapter routing, failure without local
  fallback, restoration, and isolation from another thread.

These tests extend existing unit binaries already run in CI. The fresh-guest
harness now also packages and runs those binaries and the static git-module
fixture alongside the proxy, error-shaping, and profile-parser tests. Bundled
skills are included because the existing guardrail advisory tests require them.

## Live repair discipline

The operator authorized taking over the shared hook repair. The hotfix is built
from the deployed `v0.4.1` tag with only the hook/guardrail execution repair, not
from the PR's additional thin-client and semantic-context changes. A local
Debian 13 build failed the deployment preflight against the Bookworm runtime's
older glibc; it was never installed over the running executable. The compatible
build uses the Bookworm base digest already pinned by `Dockerfile.server`, an
isolated builder with no production credentials, and reruns the focused tests.

Replacement checks the old and new executable hashes before an atomic rename.
The original executable remains at
`/usr/local/bin/aimee-server.before-hook-workspace-20260906` inside the production
server container for rollback. Credentials, policies, and persistent data are
not replaced. The temporary executable hotfix survives container restarts but
must be incorporated into the published image before a container recreation.

## Validation and deployment outcome

- Local lint: all 76 checks passed. Dispatch, utility, and guardrail unit
  suites passed, including the exact `cd --` regression. The 15 semantic-context
  release-validator/evidence tests also passed.
- Fresh-guest run `Y3BNVNfX` on `192.168.1.253`: Debian CT 9201 and VM 9202
  both passed dispatch, utility, guardrail, OpenAI error-shaping, nine profile,
  and 26 proxy tests, including real Codex streaming through the mTLS proxy.
  The harness deleted both guests and their disks. Independent checks found
  neither guest configuration nor any associated `optane` volumes afterward.
- The release-based Bookworm build passed dispatch, utility, and guardrail
  suites before installation. Installed server SHA-256:
  `0a9ee6b664c1025dec0635065468a8a45210f6fb3c49d115842da09e0c10fc3b`.
  The Docker health check and Aimee server, knowledge-store, and vector-index
  status all reported healthy after restart.
- Ordinary `git push --dry-run origin fix/thin-client-codex-proxy`, followed
  by `git push origin fix/thin-client-codex-proxy`, succeeded with the hook
  enabled. GitHub confirmed PR #2957 at `130533a370` and started fresh CI.
  CI results are tracked on the PR; local/fresh-guest results are not a claim
  that every GitHub job has completed.

The subsequent full CI run exposed a missing-shell callback in the existing
`unit-test-agent` detached-provider fixture. ASan reproduced the null call
locally. The adapter now treats a missing callback as unavailable, without
falling back to server execution; the guardrail regression matrix explicitly
covers that case. Agent and guardrail suites passed with ASan/UBSan after the
fix. The fresh-guest harness also runs the agent suite to cover this caller.
This defensive addition is in the PR; the live hotfix above uses the actual
detached provider, which supplies its shell callback.

Final expanded fresh-guest run `Ns1Vxxgx` passed on both CT 9201 and VM 9202,
including the agent suite and missing-shell regression. Both guests, disks,
and temporary payload were removed and absence independently verified. The
payload includes the bundled model catalog and `make`, and keeps test binaries
separate from the real client so the agent suite's fake clients retain their
intended PATH precedence. Those packaging requirements were found by running
the full agent suite on the minimal guests, not by weakening its assertions.
Guest agent tests retain their existing PostgreSQL/tmux availability skips;
the PostgreSQL CI shards exercise the store-backed cases. Thin-client SHA-256:
`f416efcea437761feb4d92b18baeb1a2a87b65435fb0170c3c9159ed81e6e05e`.
