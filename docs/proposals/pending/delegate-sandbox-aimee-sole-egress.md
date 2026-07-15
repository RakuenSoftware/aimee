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
- **One mount in:** the work item's worktree at `/workspace`.
- **No credentials.** The forge token, provider keys and vault stay in
  aimee-server.

Every external reach — forge, web, memory, the code index, LLM providers — becomes
an aimee tool call **by construction**. Not because a rule forbids the alternative,
but because there is no alternative to forbid.

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
