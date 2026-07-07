# Webchat git credential security model

How aimee-webchat handles a webuser's git forge credentials (at rest, in transit
to git, and inside the in-browser editor) and where exposure is and isn't
closed. This is the reference for the `webchat git projects + in-browser VSCode`
feature (proposal in `docs/proposals/done/webchat-git-projects-and-vscode.md`).

## Threat model

A webchat user (`webuser:<name>` principal) connects their own git credentials
(HTTPS PAT/token, GitHub OAuth, or an SSH key) and performs git operations and
in-browser editing on the server. The credentials are **theirs**, but aimee is
multi-user and server-hosted, so the goals are:

1. **At rest:** never plaintext on disk; encrypted per-principal.
2. **Cross-tenant:** one webuser can never read another's credential or files.
3. **In transit to git:** on the **API git paths** (clone/fetch/pull/push,
   status/log/diff/branch, and PR creation) the token must not leak into a child
   process's environment (`/proc/<pid>/environ`), argv, logs, or a named file,
   where a co-located process could read it. The **in-browser editor is
   explicitly out of scope for this invariant today**; see the editor section
   for the one remaining (bounded) exposure and the socket-askpass fix that would
   close it.
4. **No server-secret bleed:** the editor must never see aimee-server's own
   secrets (DB password, server token, provider keys).

## At rest: the per-principal vault

Credentials live only in the encrypted per-principal vault
(`server_vault.c`, `vault_service.c`, `vault_store.c`), keyed by the
`webuser:<name>` principal and sealed under the server master key
(`.server-master.key`, dual-access wrap), so the co-located server can read them
autonomously while they are never plaintext on disk and never returned to the
browser. Intake routes (`/v1/git/credentials`, `/v1/git/sshkey`,
`/v1/git/oauth/github/*`) are write-only. A `test_webchat_git_leak` check asserts
the credential's plaintext appears in **no** file under `$AIMEE_HOME`, and that a
second principal cannot read it (cross-principal denial).

The credential-resolution precedence is centralized in **one** policy
(`git_cred_inject_build_env_for_repo` / `git_cred_inject_resolve_token`); a lint
gate (`git-cred-centralized-check`) forbids any caller from hand-rolling the
ladder.

## In transit to git: the token is out of `/proc/<pid>/environ`

For **every API git path** (clone, fetch, pull, push, status/log/diff/branch, and
PR creation), the forge token does **not** appear in any child process's
environment:

- **git operations + clone** use an **fd-fed askpass**. The resolved HTTPS token
  is written to an anonymous **`memfd`** (MFD_CLOEXEC; never a named path). A
  `safe_exec` variant `dup2`s it onto a fixed fd (`GIT_CRED_TOKEN_TARGET_FD`) in
  the forked git child with CLOEXEC cleared; the env carries
  `AIMEE_GIT_TOKEN_FD=<n>` (a *number*, not the secret) instead of `GH_TOKEN`.
  The `GIT_ASKPASS` shim reads the token from `/proc/self/fd/<n>` (re-readable,
  `cat` reopens the memfd at offset 0). The source memfd is CLOEXEC in the
  parent, so a concurrent exec on another thread can't inherit it; `memfd_create`
  failure fails **closed** (drops the token) rather than falling back to env.
  Binding invariant, proven in `test_git_cred_inject`: a child run with this env
  reads the token via `/proc/self/fd/<n>` but its `/proc/self/environ` contains
  only the fd *number*, never the token.

- **Opening a PR** is an **in-process GitHub REST call** (`git_pr_api.c`), not a
  child exec: aimee-server POSTs to `/repos/{owner}/{repo}/pulls` with the token
  in the `Authorization` header, in server memory only, wiped after the call,
  never logged (the HTTP client logs host + byte counts, never headers). The
  origin host is parsed **exactly** (rejects `evilgithub.com`,
  `github.com.evil.com`, …); GitHub remotes only. (This replaced an earlier
  `gh pr create`, which read `GH_TOKEN` from the child env.)

- **SSH keys** are loaded into a per-user **in-memory `ssh-agent`** (`memfd`,
  `RLIMIT_CORE=0`, `DUMPABLE=0`, key buffer zeroed after load); git uses it via
  `SSH_AUTH_SOCK`. The agent socket lives under a **tmpfs-mandatory**,
  fail-closed per-principal runtime dir (`webuser_runtime.c`); nothing touches
  `~/.ssh`.

## Cross-tenant isolation

Every `/v1` workspace/git route resolves its target from the **attested**
`webuser:` principal (never a client-supplied path) through `workspace_scope`
(`realpath` + `openat`/`O_NOFOLLOW` containment), and rejects roots outside the
caller's own tree. The git surface can be disabled wholesale with
`AIMEE_WEBCHAT_GIT=0` (all git routes → 503); the editor independently with
`AIMEE_WEBCHAT_EDITOR=0`. Both default on.

## The in-browser editor (code-server): residual exposure

The editor process env is curated of the server's own secrets (e.g. `AIMEE_DB2_URL`,
`AIMEE_SERVER_TOKEN`, provider keys) and hardened (`no_new_privs`,
`DUMPABLE=0`, `RLIMIT_CORE=0`, loopback-only, behind the authed proxy, git's
on-disk `credential.helper` disabled). **However**, unlike the API git paths, the
editor still injects the HTTPS token as **`GH_TOKEN` in the code-server
environment.** This is the one remaining spot the token is in a child's
`/proc/<pid>/environ`. It is a **bounded** exposure: it is the user's *own*
token, in their *own* editor, but it is readable by **any same-UID code-server
descendant**: the extension host and its children (chiefly an extension the user
installs), the integrated terminal, and any shell or git it spawns. It is not yet
closed. Three pieces remain, and the key technical facts behind them were
established empirically:

### Why the editor can't use the fd-askpass (must use a socket)

The fd-askpass that works for the API git paths does **not** work for the editor.
Measured against a real code-server 4.126 / Node 24 on a test host:

- VS Code's **SCM "Git" view** runs git in the **extension host** via
  `child_process.spawn`, which **does** forward an inherited extra fd, so an
  fd-askpass would work there.
- The **integrated terminal** spawns its shell via **`node-pty`**, which **does
  not** forward the inherited fd (the dup2'd fd is not open in the pty child; node-pty does not inherit it).

So switching the editor to fd-mode would silently **break terminal git** (the
askpass would find no fd and, with no `GH_TOKEN` to fall back to, fail auth). The
correct fix is a **socket-fed askpass**: a per-principal unix-domain socket
(0700, in the tmpfs runtime dir, like the ssh-agent) served from aimee-server's
in-memory broker; the askpass connects and reads the token, which works
regardless of fd inheritance. This is scoped but unimplemented.

### Extension-host containment is not a selective env-strip

The proposal's "strip `GIT_ASKPASS`/`SSH_AUTH_SOCK` from the extension host but
keep them for the integrated terminal **and SCM**" is **internally
contradictory**: VS Code's SCM Git provider *runs in the extension host*, so
those are the same trust boundary. There is no knob to give the built-in Git
extension the credential env while denying it to a third-party extension in the
same host. The realistic mitigations are: the secret-free curated env (done, no server
secrets reach any extension), an operational posture of Open-VSX-only with no
preinstalled third-party extensions (a deployment policy, not code-enforced), and
treating any extension the user installs as having access to the user's *own* git
credential by design.

### OS-jail (namespace / seccomp / cgroup) is future work

The editor terminal currently runs as the server UID with the server `PATH`
(`no_new_privs` already neuters setuid escalation such as `sudo`, and the
non-root UID denies `docker`/`nsenter`). A stronger jail (a `bwrap`/`unshare`
bind-mount namespace confining the terminal to the workspace subtree, a `seccomp`
filter denying `mount`/`pivot_root`/`setns`, and cgroup CPU/mem limits) is
future work; it requires `bubblewrap` in the image and iterative validation
against a live editor.

## Status summary

| Surface | Token in `/proc/<pid>/environ`? | Mechanism |
|---|---|---|
| clone, fetch, pull, push, status/log/diff/branch | **No** | memfd fd-askpass |
| open PR | **No** | in-process GitHub REST (`Authorization` header) |
| SSH git | **No** | in-memory ssh-agent (`SSH_AUTH_SOCK`) |
| in-browser editor (code-server) | **Yes** (bounded: user's own token) | `GH_TOKEN` in env; fix = socket-fed askpass (scoped) |

**Remaining hardening** (all editor-scoped, all needing a live code-server to
implement+validate): socket-fed askpass for the editor; the bwrap/seccomp/cgroup
jail. Extension-host "containment" is resolved as a documented limitation rather
than an (infeasible) selective env-strip.
