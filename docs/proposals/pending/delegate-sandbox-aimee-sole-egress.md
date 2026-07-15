# Proposal: Delegate sandbox — aimee-server as the sole egress

- **State:** proposed (pending — not started)
- **Operator ruling:** 2026-07-15. "Delegates just spawn in a custom container… we
  will use aimee-server as the delegate's external communication layer, there
  should be no network communication with the delegate containers except for
  aimee server."
- **Depends on:** PR #1352 (delegates have aimee's git tools), and
  [hashline edit + lean websearch](proposal-hashline-edit-and-lean-websearch.md)
  Part II (aimee's own `web_search` / `web_read` with a hardened egress policy).

## Thesis

A delegate should not be *asked* to route its work through aimee. It should have
no other option.

Today the boundary is advisory. `wfe_shell_invokes_git()` is a string match over a
shell line; `delegate_child_strip_forge_creds()` removes the environment variables
we know how to name. Both are useful, and neither is a boundary — the classifier's
own header says so:

> Treat a NEGATIVE result as "no KNOWN externalization pattern", NEVER as "provably
> safe". … The full seal would require a sandbox (no network / read-only mounts
> outside the worktree), tracked separately.

This is that sandbox. It converts every rule we currently *state* into a property
of the environment: the delegate cannot reach the forge because it holds no
credential and has no route; it cannot reach the web because it has no network
stack. Not "must not" — **cannot**.

## The change

A delegate runs in its own container with:

- **`--network none`.** No network stack at all. Not a firewall rule, not an egress
  allowlist — no interface to configure or evade.
- **One channel out:** `<AIMEE_HOME>/aimee-http.sock` bind-mounted in. A Unix
  socket, so it survives `--network none`, and aimee-server is the only thing on
  the other end.
- **One mount in:** the **entire current source tree** at `/workspace` — read-write
  for a delegate that edits (its own worktree), read-only for a reviewer. See
  "The whole tree, not a fragment" below; this is not a concession to convenience,
  it is what makes the delegate and the reviewer able to do their jobs at all.
- **No credentials.** The forge token, provider keys and vault stay in
  aimee-server.

Every external reach — forge, web, memory, the code index, LLM providers — becomes
an aimee tool call **by construction**. Not because a rule forbids the alternative,
but because there is no alternative to forbid.

## The whole tree, not a fragment

The container gets the **entire current source tree**. This is the part that pays
for itself twice, because today both halves of the fleet are working half-blind.

**A background delegate is handed an empty directory.** Not a metaphor — the
workspace it gets says so, in a note the code writes into the workspace itself
(`delegate_ephemeral_ws.c`):

> This is a server-side ephemeral git workspace for a background aimee delegate.
> The dispatching client disconnected, so the client's repository is **NOT present**
> here — file/shell tools run against this **initially-empty checkout**. A background
> code delegate that must edit the client tree **needs the repo provisioned here**.

It is `git init`-ed for one reason: the write-guard permits writes only inside a git
checkout, so a plain directory would block every edit. The delegate can therefore
write — into nothing. The note is an admission, left for whoever came next.

**A reviewer is handed no filesystem at all.** `review_indexed` deliberately
excludes `read_file`/`grep`/`list_files`, and the stated reason is that they "point
at a worktree a remote delegate cannot reach". That reason is an artifact of where
the reviewer runs, not a judgement that reviewers shouldn't read code. So a panel
judging a diff cannot open the file the diff is in. It reasons from the patch text,
the code index, and inference — which is exactly how a review passes a change whose
`git_commit` advertises parameters its handler never accepted, and how twelve
rt_gate iterations still left holes. We have been asking panels to review code they
cannot read.

Mounting the tree dissolves both. The reviewer's exclusion is not a policy to keep
— it is a workaround for a missing mount, and once the mount exists the workaround
should go: `review_indexed` gains read-only `read_file`/`grep`/`list_files`, and a
reviewer can answer "is this reachable?" by looking instead of hedging.

The isolation story is unchanged, because the tree was never the perimeter. The
perimeter is `--network none` plus no credentials. Source code is not a secret from
a delegate that is about to edit it; the forge token is. What the mount changes is
whether the agent can see its own subject.

- **implement** — its worktree, read-write. Writes stay anchored at `/workspace`;
  absolute paths and `..` are still rejected by the backend.
- **review** — the tree at the PR's branch, **read-only**. A reviewer that cannot
  write cannot "fix" what it was asked to judge.

## §0 What already exists (do not rebuild)

| Already built | Where |
|---|---|
| Container lifecycle: create / start / exec / stop / rm, hibernate-on-exit | `src/server/delegate_backend_docker.c` (697 lines), **registered** at `server.c:2059` |
| Workspace anchoring: `-v <workspace>:/workspace -w /workspace`, absolute paths and `..` rejected | same |
| File ops through `docker exec` (read/write/list, b64-wrapped) | same |
| The `/v1` Unix socket the sandbox talks to | `server_http.c` — always served, no TCP port needed |
| Delegates reaching aimee: `git_commit` / `git_push` / `git_pr` server-side; `aimee mcp serve` as the CLI delegate's MCP server; `AIMEE_API_ENDPOINT=unix:…/aimee-http.sock` | PR #1352 |
| Hardened server-side egress (http/https only, resolved-IP deny-list, per-hop redirect re-validation, connection pinned to the validated IP) | [lean websearch](proposal-hashline-edit-and-lean-websearch.md) Part II |

PR #1352 is load-bearing here, not merely adjacent: a credential-less delegate in a
network-less box **could not commit at all** before it. The tools had to exist
first. Same ordering applies to everything below.

## The gap

`td_bash` (`src/posix/agent_tools_dispatch.c:328`) does not use a backend. It
routes to the detached workspace provider, else falls through to `run_cmd` —
**in-process, inside the aimee-server container**. The docker backend's `exec()` has
exactly one caller: `server.c:499`, a `delegate.*` RPC.

So the isolation is built and wired to the wrong thing. The delegate that matters —
the wfe `implement` delegate, which is the native agent
(`wfe_live_delegate.c:136`) — runs its shell inside aimee-server, with the server's
filesystem and environment.

**The work:** route `td_bash`, `execute_script`, `td_write_file`, `td_read_file`,
`td_list_files` through the backend seam. Roughly the shape of the two seams PR
#1352 added.

## Aimee must own the tools first

`--network none` takes capabilities away. Each one has to exist on aimee's side
**before** the network goes, or the sandbox is just breakage — the same mistake as
forbidding `bash git` while native agents had no `git_commit`.

| Delegate loses | aimee must provide | Status |
|---|---|---|
| `WebSearch` / `WebFetch` (Claude Code's own, network-bound) | `web_search` / `web_read` | **Proposed** — lean websearch Part II, rev. 4, three roundtable rounds, "ship with changes" |
| `curl` / `wget` | (same) | as above |
| `git` / `gh` | `git_commit` / `git_push` / `git_pr` / `git_branch` | **Done** — PR #1352 |
| memory, code index | `search_memory`, `code_search`, `find_symbol` | Already native builtins |
| package installs (`npm i`, `pip install`) | — | **Open.** See below. |

Two consequences worth stating plainly:

1. **The lean-websearch proposal stops being an optimisation and becomes a
   dependency.** Its SSRF hardening also gets more load-bearing, not less:
   aimee-server becomes the *sole* egress for every delegate, so its egress policy
   is the whole perimeter. That proposal already anticipated this (resolved-IP
   deny-list, per-hop redirect re-validation, pinning the connection to the
   validated IP against DNS rebinding).
2. **The CLI delegate's `--allowedTools` must drop `WebFetch`/`WebSearch`** in
   favour of the `mcp__aimee` equivalents; Claude Code's built-ins would simply
   fail in a network-less container.

## Open questions

- **Toolchain in the image.** `verify` needs to build and run tests. This is
  already live and unsolved: `git_verify` fails on .254 for want of a C toolchain,
  and an operator `project.yaml` works around it with `bash -n` / `py_compile`. The
  sandbox forces the question rather than creating it — and arguably improves it: a
  purpose-built delegate image can carry the toolchain the server image should not.
- **Package installs.** A build that fetches dependencies needs egress. Options: a
  pre-baked image per project; an aimee-mediated package proxy; a narrowly-scoped
  registry exception. Unresolved, and the most likely source of "the sandbox broke
  my build".
- **Docker socket.** aimee-server needs one. On .254 that is the tierd private
  daemon (`unix:///run/smoothnas-runtime/docker.sock`) — a docker-in-docker
  question, and handing a container the docker socket is itself a privilege
  boundary worth thinking about.
- **`set_cwd` persistence** is documented as only partial in the docker backend.
- **Cost / latency:** a container per delegate; `hibernate_on_exit` exists.

## Risks

- **Breaking `implement`.** Every capability removed must land on aimee's side
  first. Ordering is not a nicety — it is the whole lesson of PR #1351, which
  forbade a route to tools that did not exist.
- **The docker socket as a new escape hatch.** A delegate that reaches it owns the
  host. It must never be mounted into the delegate container — only aimee-server
  holds it.
- **Egress policy becomes the perimeter.** Every hole in aimee's `web_read` is now
  a hole for every delegate.

## Acceptance

The ordering is the risk, so the criteria pin it: every capability must be proven
to exist on aimee's side *before* the check that removes it from the delegate.

```yaml acceptance
- {id: 1, tier: integration, check: "a native delegate commits + pushes via git_commit/git_push with NO forge credential in its environment"}
- {id: 2, tier: integration, check: "aimee web_search/web_read return results for a delegate that has no network stack (lean-websearch Part II landed)"}
- {id: 3, tier: mechanical, check: "td_bash/execute_script/td_write_file/td_read_file/td_list_files dispatch through the delegate backend seam, not run_cmd"}
- {id: 4, tier: deployment, check: "delegate container runs with --network none; `curl https://api.github.com` inside it fails with no route, not with an auth error"}
- {id: 5, tier: deployment, check: "the only mount besides /workspace is aimee-http.sock; the docker socket is NOT mounted into the delegate container"}
- {id: 6, tier: deployment, check: "a delegate resolves nothing outside /workspace: absolute paths and .. are rejected by the backend"}
- {id: 9, tier: integration, check: "a delegate's /workspace is the FULL source tree, not an empty checkout: it can read a file it never wrote and that the diff never touched (today delegate_ephemeral_ws hands background delegates a git-init'd empty dir whose own AIMEE_WORKSPACE_NOTE.txt says the repo is not present)"}
- {id: 10, tier: integration, check: "a REVIEW delegate resolves read_file/grep/list_files and can open a file named in the diff — review_indexed's filesystem exclusion exists only because the worktree was unreachable, and must be lifted with the mount, not kept"}
- {id: 11, tier: deployment, check: "a review delegate's /workspace is mounted READ-ONLY: a write from a reviewer fails at the mount, not at a guard it could be talked out of"}
- {id: 12, tier: integration, check: "AIMEE_WORKSPACE_NOTE.txt (the 'repository is NOT present' admission) is GONE, not merely stale — if a workspace can still be empty, this proposal has not landed"}
- {id: 13, tier: integration, check: "ISOLATION: a delegate's writes never reach a tree it shares. Its own worktree mounts rw; a shared tree mounts :ro at the docker mount, not merely behind the write guard. A write-capable delegate with no worktree of its own is left UNSANDBOXED rather than handed a read-only tree it cannot use"}
- {id: 14, tier: integration, check: "the mounted tree is bounded by the operator's REGISTERED workspace roots, canonicalized first. Repository-ness is NOT authorization (`mkdir .git` satisfies it); a root of `/` authorizes nothing"}
- {id: 15, tier: deployment, check: "a container mounting the caller's real tree runs as the server's uid:gid, so a delegate cannot leave root-owned files in the user's checkout (git would then refuse the tree for dubious ownership)"}
- {id: 7, tier: hardware, check: "the wfe implement stage completes end-to-end on .254 with the sandbox active, including verify"}
- {id: 8, tier: integration, check: "with the sandbox active, wfe_shell_invokes_git's documented evasions (base64, subshell, env indirection) reach the forge in NEITHER case — belt-and-braces, not the defence"}
```

## Why this is worth it

Today's defences are a string match and an environment scrub, and the honest
assessment is in the code: obfuscation the classifier cannot see (subshells,
base64, env indirection) evades it, and the credential strip removes only what we
thought to name. After this, those become belt-and-braces. The delegate does not
push because it has no credential and no route — and the rule that says "use
aimee" stops being a rule and becomes a description of the only thing that works.
