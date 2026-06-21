# Autonomous Development Execution Substrate: build · verify · push · validate · accept

- **State:** proposed; awaiting roundtable + user proposal-gate.
- **Scope:** deterministic / autonomous-dev plumbing. Not an intelligence-surface
  proposal (no Architecture Charter role). It is the execution substrate that
  [full-autonomous-development.md](full-autonomous-development.md) assumes but does
  not itself provide.
- **Author:** JBailes (drafted by the engineer agent during an autonomous
  proposal-completion run, 2026-06-21).
- **Origin:** a live attempt to drive `docs/proposals/pending/` to completion
  fully autonomously. The development loop itself works — one slice was taken
  plan → roundtable → implement → roundtable → verify → PR → green CI → merge
  (ingress-compression P0, PR #585). But the run could **not** be sustained
  end-to-end without human intervention, and several proposals are **structurally
  unclosable** in the agent's environment. This proposal names those blockers and
  fixes them.

## Problem — why "complete all proposals autonomously" currently fails

Concrete blockers hit during the run, in order of severity:

1. **No in-environment build/verify.** The server/webchat workspace has no
   toolchain — `make`, `gcc`/`cc`/`clang`, and `docker` are all absent. A C change
   cannot be compiled, unit-tested, or linted where the agent runs. The run only
   proceeded because a human hand-provisioned an external Proxmox build host and
   pasted an SSH key. **Mechanical verification is impossible in-env**, so the
   agent's own Code Principle ("verify with the most relevant tests") is
   unsatisfiable without manual setup.

2. **No first-class authenticated push/PR.** The repo is private and no git
   credentials are configured. The running server's `git_ops` push path does
   **not** use the vaulted forge token (only `clone` does). To open PR #585 the
   agent had to **manually decrypt the server-principal PAT** (HKDF-SHA256 → AES-KW
   unwrap → AES-256-GCM) with Node and drive a credential helper by hand. An
   autonomous loop cannot depend on a human, or on bespoke crypto, to ship.

3. **A whole class of acceptance criteria is unclosable in-env.** Many proposals
   gate on **deployment / real-hardware** validation the agent cannot perform:
   Windows/macOS `https://` connect (native-tls, mtls slice 3b),
   live-server re-provision on the `.254` box (auto-vault criterion 5), a running
   embedder+kb stack (embedder-runtime-fetch-autodim §2), multi-arch image
   publish (unified-llm-container, curator-llm). There is no harness the agent can
   drive to perform — or formally defer — these checks, so those proposals can
   never reach a defensible "done" autonomously.

4. **No machine-checkable definition of done.** Acceptance criteria are prose
   requiring human judgment. The agent cannot deterministically decide
   "this proposal is complete," so it must either over-claim (dishonest) or stall
   (not autonomous). Half of `pending/` turned out to be **already shipped but
   never filed to `done/`** — there is no reconciliation between implemented code
   and proposal state, so the loop burns effort re-triaging.

5. **Premise drift between proposals and the tree.** A proposal's plan can assume
   a code shape that no longer exists (e.g. ingress-compression P1a specifies a
   "blank-line/whitespace collapse" fold, but the resident code form is already a
   single collapsed line — the fold is a no-op). Nothing keeps proposals grounded
   against the evolving tree, so an autonomous implementer hits design forks that
   silently need a human.

6. **Review-gate reliability/throughput.** The roundtable is the designated review
   gate, but it degrades on large prompts (a ~22 KB review returned 0 items,
   deadline-hit; ~12–16 KB ran a single round), is slow (~5–7 min/run), and 401s
   when the delegate vault is empty — i.e. the review capability itself depends on
   blocker (2)/auto-vault. A loop that reviews many proposals needs a bounded,
   reliable gate.

**Net:** the *intelligence* to develop proposals is present; the **execution
substrate** to build, verify, ship, and certify them autonomously is not. Without
it, autonomous completion stops at the first compile, the first push, or the first
deployment-tier acceptance criterion.

## Goal

Give the autonomous driver (and aimee's engineer delegates) a first-class,
in-process substrate so that, for any proposal whose acceptance criteria are
mechanically or integration-checkable, the system can go from intake to a merged
PR and a `done/` filing **with zero human steps** — and, for criteria that
genuinely require deployment or physical hardware, **dispatch the existing CI/CD
matrix and gate on it**, or stop at an explicit, labelled validation gate rather
than guessing.

## Design

Five components. Each is independently shippable; together they close the loop.

### §1 Build-and-verify runner (fixes blocker 1)

A callable verify gate the driver invokes on a work-item worktree, returning
structured `{tier, step, status, log_ref}` — never a human shell.

- **Runner image** — a pinned image carrying the CI toolchain (`gcc make
  clang-format-19 libpq postgresql-client …`, mirroring `.github/workflows/ci.yml`)
  so a run reproduces CI locally.
- **Two provisioning modes**, config-selected: (a) a **managed runner pool**
  (long-lived container/CT, reused across runs) or (b) an **ephemeral provisioner**
  that stands a runner up per job and tears it down — the Proxmox-CT / `docker run`
  pattern proven manually during the run, formalised behind one interface.
- **Steps** = `build` (`make -j all server`), `unit` (`make unit-tests`), `lint`
  (`make lint`), each with a structured pass/fail + captured log. This is the
  MECHANICAL tier of §3 and the thing `exec_implement`'s "verify each unit" step in
  full-autonomous-development actually calls.
- Default-off; when no runner is configured the gate reports `unavailable`
  (degrade, not a false pass) so nothing is ever reported verified that wasn't.

### §2 Vault-backed forge push + PR (fixes blocker 2)

Close the `git_push`-ignores-the-vault gap so shipping needs no human and no
ad-hoc crypto.

- `git_ops` push and the forge vtable's `open`/`merge` resolve the
  **server-principal** `git` / `host:github.com` credential through the existing
  `vault_service_get_server_wrap` path and inject it via a credential helper —
  the same token the agent decrypted by hand for PR #585, now used in-process.
- Honour branch policy automatically: cut PR branches off `origin/testing`,
  **squash** to `testing` with the `(#NNN)` convention, **no `Co-Authored-By`
  trailers** (the required `no-coauthor-trailers` check), human-only promotion to
  `main`. Every push/PR/merge audited and attributed to the work item.

### §3 Validation tiers + deployment harness (fixes blocker 3)

Make "done" decidable even when the agent can't physically perform a check.
Classify every acceptance check into a tier and route it:

| Tier | Examples | Where it runs |
|------|----------|---------------|
| `mechanical` | build, unit, lint | §1 runner, in-loop |
| `integration` | live server/kb/db2 e2e, migrate smoke | ephemeral stack (§1 runner + postgres), in-loop |
| `deployment` | multi-arch image publish, `.254` re-provision | dispatch the GitHub Actions matrix / release pipeline; gate on its conclusion |
| `hardware` | Windows/macOS `https://` connect, real-device run | dispatch the platform CI legs; if a check needs physical hardware CI can't supply, it becomes an explicit, tracked `validation-pending` gate — never auto-claimed |

The driver runs `mechanical`/`integration` itself, **dispatches** `deployment`/
`hardware` to CI/CD (it can already trigger and read the Actions matrix that
gates every PR), and surfaces any `hardware` check no runner can satisfy as a
labelled human gate. No tier is ever silently marked passed.

### §4 Machine-checkable acceptance (fixes blocker 4)

An optional `acceptance:` block in a proposal's front matter — a list of
`{id, tier, check}` where `check` is a runnable command / test id / CI job name.

```yaml
acceptance:
  - {id: 1, tier: mechanical,   check: "make unit-tests TEST=test_vault_bootstrap"}
  - {id: 5, tier: deployment,   check: "ci:e2e-docker"}
```

The driver evaluates each check on its tier (§3) and computes a deterministic
verdict. **All-green across the applicable tiers ⇒ the proposal is auto-filed to
`done/`** (with the link-graph fixups `check-proposal-links` requires); a partial
result reports exactly which `id` is open and why. Proposals keep their prose
criteria; the block is the executable shadow of them. Back-fill is incremental —
unannotated proposals fall back to today's human judgement.

### §5 Proposal ↔ code reconciliation (fixes blockers 4, 5)

A reconciler (`aimee dev reconcile`, also a lint check) that:

- flags **shipped-but-unfiled** proposals (acceptance block all-green but
  `state ≠ done`) — this run found ~6;
- flags **premise drift** — a proposal/plan that names a symbol, path, or
  function that no longer resolves in the tree (e.g. a fold over a code shape that
  changed), so design forks surface as a report instead of a silent stall;
- keeps `docs/PROPOSALS.md` honest against the directory state.

## Acceptance criteria

1. The driver builds + unit-tests + lints a work-item worktree through one callable
   verify gate with **no human-provisioned host**, returning structured per-step
   results; absence of a runner reports `unavailable`, not a pass.
2. The driver pushes a branch and opens **and** squash-merges a PR to `testing`
   using the **vaulted** forge credential — no manual token decryption, branch
   policy + `no-coauthor-trailers` honoured, all actions audited.
3. Acceptance blocks are machine-evaluated per tier; a fully-green proposal is
   auto-filed to `done/` with link-graph fixups; `deployment`/`hardware` tiers are
   dispatched to and gated on the CI matrix, and a check no runner can satisfy
   becomes a labelled `validation-pending` gate (never auto-claimed).
4. The reconciler detects at least the shipped-but-unfiled and premise-drift
   classes and lists them with the offending evidence.
5. **End-to-end:** a proposal submitted via full-autonomous-development intake,
   whose acceptance is entirely `mechanical`/`integration`, reaches a merged PR +
   `done/` filing with zero human steps; one with a `deployment`/`hardware` tier
   stops at an explicit, labelled gate with the CI dispatch recorded.

## Risks

- **Arbitrary build execution.** The runner executes repo build scripts — sandbox
  it (no network beyond package mirrors, no secrets mounted into the build env,
  ephemeral FS) so a poisoned build can't exfiltrate or persist.
- **Forge-credential blast radius.** The push path holds a `repo`-scoped PAT —
  keep it server-principal-only, audit every use, never log it, prefer a
  short-lived/installation token where possible.
- **Over-trusting machine acceptance.** Promotion to `main` stays human; the
  `deployment`/`hardware` tiers are never auto-claimed; a green mechanical tier is
  necessary, not sufficient, for those proposals.
- **Reconciler false positives** on premise drift (a renamed-but-equivalent
  symbol) — report-only, never auto-edits a proposal.
- **Runner ≠ CI drift.** Pin the runner image to the CI toolchain versions and
  fail the gate if they diverge, so a local pass that CI would fail can't ship.

## Relationship to existing proposals

- [full-autonomous-development.md](full-autonomous-development.md) owns the **wfe
  lifecycle, scheduler, and intake** — the *control plane*. This proposal owns the
  **build/verify/push/validate/accept execution plane** that control plane drives.
  full-autonomous-dev's `exec_implement` "verify via mechanical/review/adversarial
  gates" and `exec_pr_open` "git push + forge open" are exactly §1 and §2 here;
  today they assume plumbing that does not exist.
- The shipped auto-vault-provisioning work (a populated delegate vault) is a
  prerequisite for §2/§6 so the review gate and forge credential resolve; it
  shipped, which is why this run could review at all.
- The roundtable throughput/reliability limits (blocker 6) are tracked by the
  roundtable-reliability work and the review-prompt-size discipline; this proposal
  depends on a working review gate but does not re-solve it.
