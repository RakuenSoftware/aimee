# Proposal: mirror-sync drops a client diff once the delta gets big enough

- **State:** OPEN — reproduced and bounded, not diagnosed. Found while
  migrating db1's ensemble family; nothing here is caused by that work.

Six integration checks around `MCP Git` and the workspace mirror fail whenever
the local branch carries a large enough unpushed delta under `src/`. They pass
again the moment the branch is pushed, which is why this has stayed invisible:
the suite is usually run just after a push, when the delta is empty.

## What actually happens

`cli_workspace_reverse_channel_sync` ships the client's working-tree patch to
`/v1/workspace/mirror-sync` so the server's reconstructed sandbox matches what
the developer actually has. With a large delta that POST fails with **HTTP 0** —
no response at all — and the client prints:

    aimee: could not ship the working-tree diff for <root> (HTTP 0); the
    server-side sandbox will be a clean checkout at HEAD and will NOT contain
    uncommitted work

`cli_mcp_serve.c` then fails closed on every `git` tool call, which is correct
behaviour on its part and is what the six checks report.

## Why it is not what it first looks like

**It is not the code under test.** The failure reproduces with a byte-identical
binary by moving only the local tracking ref:

    git update-ref refs/remotes/origin/<branch> <older-commit>

The base for the patch is the pushed upstream commit, so moving that ref alone
changes the shipped diff and nothing else. Same server, same client, same test:
six failures. That is the whole proof that no compiled behaviour is involved.

**It is not simply size.** A single new 166KB text file as the entire delta
ships fine. A 130KB delta spread across 19 source files does not.

**It is not the file count.** Twenty new files totalling 4020 lines ship fine.

**It is cumulative across the delta.** Splitting the 19 files into halves of
roughly 65KB each, either half ships fine; both together do not. So no single
file is responsible, and something aggregate is.

**It is not a declared body limit.** The assembled JSON body is about 139KB.
`SHTTP_MAX_BODY` is 4MB and an oversize body would return 413, not close the
connection. `cli_http_build_request` grows its buffer with the body, and its one
failure condition compares the Authorization header against the raw bearer --
independent of body length, so it would fail for every request or none.

That exhausts the cheap explanations. What is left is the write or the read on
the socket: HTTP 0 is also what the client reports when the server closes the
connection mid-request, which is the symptom `cli_v1_routes_internal.h` already
warns about ("dropped by the listener before it is parsed, which the client can
otherwise only report as 'could not reach the endpoint' -- blaming a server that
is up and answering").

## Why it matters beyond the test

The six red checks are the small half. The real cost is that this is exactly the
path whose job is to carry uncommitted work to the server-side sandbox, and its
failure mode is a warning line on stderr and a sandbox that is quietly a clean
checkout at HEAD. An agent then works on a tree that is missing the developer's
changes, and nothing downstream says so. The bigger the unpushed work, the more
likely it is to be missing -- which is the wrong way round.

## Reproducing

    git update-ref refs/remotes/origin/<branch> <commit ~130KB of src/ behind>
    cd src && ./tests/test_integration.sh

The harness can be run directly, without `make`, which makes each iteration
about a minute. Restore the ref afterwards with `git fetch origin <branch>`.

## What to look at first

Whether the listener drops the connection on a body that is large but legal, and
whether `mirror-sync`'s chunked path (`server_runner_endpoints.c` already
reassembles a chunked patch behind a transfer id) is what the client is supposed
to be using above some size. `rc_ship_client_diff` sends the patch whole and
never sets `seq`/`final`, so if chunking is expected past a threshold, the client
is simply not doing it -- which would fit every observation above, including why
two halves succeed where the whole fails.
