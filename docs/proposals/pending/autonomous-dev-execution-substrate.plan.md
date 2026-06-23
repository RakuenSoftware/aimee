# Implementation plan: autonomous-dev execution substrate

Companion to [autonomous-dev-execution-substrate.md](./autonomous-dev-execution-substrate.md)
(**APPROVED**, human proposal-gate 2026-06-21). Base branch: `testing`. Each packet is
**independently shippable as its own PR** — built/linted/tested + roundtable-reviewed +
merged before the next, per the autonomous-proposal loop the proposal itself describes
(§Origin). Smallest/safest, highest-leverage-with-no-new-infra first.

## Grounding against the current tree (2026-06-23)

The proposal was drafted in a webchat workspace with **no toolchain**; this plan is
grounded in a checkout that **has** `make gcc clang clang-format-19 psql node gh`
(docker absent). That changes the packet order: the pure-code reconciler/acceptance
work (§4/§5) is **fully build-and-test-verifiable in-env today**, so it ships first and
de-risks every later packet by making proposal↔tree drift machine-visible. Two further
facts pin the sequencing:

- **§2 is mostly already shipped.** `src/server/git_ops.c` already resolves credentials
  vault-first for fetch/pull/**push** via `git_cred_inject_build_env_for_repo`
  (`needs_cred=1` on push, PR #605). The residual §2 work is the **forge vtable**
  `open`/`merge` path + per-action audit, not the push gap the proposal describes.
- **§1/§3 need a runner/CI-dispatch substrate** (container or Proxmox-CT); they are only
  *partly* verifiable in-env. They follow the pure-code packets.

## Packet sequence

| Packet | Component | New infra? | In-env verifiable? |
|--------|-----------|------------|--------------------|
| **P1** | §5 reconciler (static core) + §4 acceptance-block schema | none | **fully** |
| P2 | §2 finish: forge vtable `open`/`merge` vault + audit | none | unit + stub |
| P3 | §1 build-and-verify runner (ephemeral + managed modes) | runner image/CT | partial (managed mode locally) |
| P4 | §3 validation tiers + CI-dispatch harness | CI dispatch | partial (dispatch unit-testable) |
| P5 | §4 full per-tier acceptance evaluation + §5 auto-file shipped-but-unfiled | P3+P4 | integration |

P1 is the only packet detailed here; later packets get their own plan revision once P1's
reconciler makes the tree's real drift visible (it may re-scope P2–P5).

---

## P2 — route every git op through the ONE credential policy (§2) — THIS PR

**The defect (architectural).** Aimee already has a single credential-resolution policy:
`git_cred_inject_build_env_for_repo(principal, remote_url, repo_dir, preferred_token, env)`
— documented as *"the ONE credential-resolution policy every git network op routes through,
so the precedence can never drift between call sites"* (precedence: `preferred_token`
[inline / workspace broker §4] → per-host **server vault** → `principal` **webuser vault**
→ server identity [App-token / `AIMEE_FORGE_TOKEN`]; injects `GH_TOKEN` + `GIT_ASKPASS` +
`GIT_TERMINAL_PROMPT=0`, wipes the token). `git_ops.c`, `git_project.c`, `webuser_editor.c`
already route through it.

But **two call sites bypass it and hand-roll their own ladder**, re-deriving the precedence
(and so drifting from it — they omit the webuser-vault rung, and neither is auditable as a
single point):
- `mcp_git_run` (`src/mcp_git_query.c`) — the executor for **every `gh pr create/merge`**
  (and clone/fetch/push) in the shared-provider path.
- `ws_mirror_git_runner` (`src/server/workspace_turn.c`) — the workspace-mirror git runner.

Both do `forge_cred_get(broker) → git_host_resolve_token(per-host) →
forge_cred_build_server_env(identity)` by hand. **No downstream caller should resolve a
token.** (Directive, 2026-06-23: *Aimee handles git creds, no downstream processes. Full
stop.*) The earlier draft of this packet proposed teaching `forge_cred_server_identity` to
read the vault — that was the wrong layer (it spreads token knowledge) and is dropped.

**Premise-drift the reconciler would flag:** §2 also describes the push gap as still-open
and names `vault_service_get_server_wrap` as the accessor. Both are stale — push was
centralised by #605, and the server's OWN token is read via
`vault_service_get_server_principal` (get_server_wrap is for a webuser dual-wrap entry; see
`git_host_cred.c`). No code change needed for those; noted so the proposal can be corrected.

### Change

Route both call sites through the one policy, passing the workspace broker token as
`preferred_token`. This **deletes** the hand-rolled ladder at each site (the
`git_host_resolve_token` + `forge_cred_build_server_env` + `forge_cred_build_env_from_token`
calls) and replaces it with a single `git_cred_inject_build_env_for_repo(...)` call:

```c
char tok[FORGE_TOKEN_MAX] = {0};
const char *pref = NULL;
if (wsid && wsid[0] && forge_cred_get(wsid, (long)time(NULL), tok, sizeof tok) == 0 && tok[0])
   pref = tok;                       /* broker token must win — it is preferred_token */
char **envp = git_cred_inject_build_env_for_repo(NULL, remote, repo_dir, pref, environ);
secure_wipe(tok, sizeof tok);        /* caller still wipes its own copy */
/* envp ? exec under envp + free_env : ambient */
```

- `mcp_git_run`: `remote=NULL`, `repo_dir=cwd` (the policy reads `origin` itself, matching
  today's `git_host_resolve_token(NULL, cwd)`).
- `ws_mirror_git_runner`: `remote=rctx->remote`, `repo_dir=NULL`.
- `principal=NULL` keeps behaviour **identical** to today (the webuser-vault rung was never
  reached at these sites); wiring a real turn principal — to additionally honour a webchat
  user's own vaulted token — is a deliberate follow-up, not smuggled into a refactor.

Behaviour is preserved exactly (broker → per-host vault → server identity → ambient), but
now there is **one** resolver, so precedence can never drift again and a single audit/policy
point exists for the driver packet. Net: ~20 lines of duplicated credential logic removed.

### Tests (fully in-env)

- `src/tests/test_mcp_git.c` (or a focused `test_git_cred_inject_routing.c`): assert
  `mcp_git_run`/the runner builds the git child env via the one policy — with a registered
  per-host vault token the child env carries `GH_TOKEN`+`GIT_ASKPASS`; with a broker token
  installed it wins; with neither, envp is NULL (ambient). Driven through the existing
  test seams (`git_host_resolve_register`, `forge_cred_install`) + a captured-exec stub, so
  no live forge is touched.
- A regression assertion that **no `forge_cred_build_server_env` / `git_host_resolve_token`
  call remains** in `mcp_git_query.c` / `workspace_turn.c` (grep gate in the test or a
  module-boundary check) — locks in the centralisation.

### Deferred, explicitly labelled

Audit + work-item attribution and branch-policy enforcement (base-off-`testing`, squash,
no-coauthor) belong to the **driver packet P4**, where the work-item context lives — and the
one policy is now the single place to hook them. The **live-forge e2e** (`gh pr create/merge`
against GitHub with the vaulted token) needs network egress + a populated server vault and is
a `deployment`-tier `validation-pending` check, never auto-claimed.

### Acceptance block (dogfoods P1's §4 gate)

```yaml acceptance
- {id: 10, tier: mechanical,  check: "make unit-tests TEST=test_mcp_git"}
- {id: 12, tier: deployment,  check: "ci:live-forge-pr-roundtrip"}
```

---

## P1 — reconciler static core + acceptance-block schema (shipped, #639)

A single Python lint gate, `scripts/reconcile-proposals.py`, in the established
`scripts/check-*.py` + `--plant-test` pattern (mirrors `check-proposal-links.py`,
wired into `make lint`). No C build needed; runs under `python3` directly. Three checks,
all decidable from the working tree alone — no runner, no CI, no live stack:

### Check A — state↔folder consistency (the safe slice of "shipped-but-unfiled", §5)

The **folder is authoritative** for a proposal's lifecycle state (`pending/` `accepted/`
`done/` `rejected/` `deferred/`). The `- **State:**` bullet is prose and drifts. Rather
than substring-match free prose, the State bullet is **classified into a closed enum** by
case-insensitive keyword (the literal keyword sets live at the top of the script so the
implementer and reviewer agree):

- `terminal_done` — bullet contains `done` / `shipped` / `merged` / `deployed`.
- `terminal_closed` — `rejected` / `withdrawn` / `superseded`.
- `in_flight` — `draft` / `proposed` / `pending` / `accepted` / `approved` / `reviewed` /
  `ready` / `in progress` / `blocked` / `partial`.
- `unknown` — no keyword matched, or **no State bullet at all**.

`done`/`deployed` keywords win over `in_flight` ones when both appear (a bullet like
"merged to testing and deployed; partial rollout" classifies `terminal_done`). The folder
defines the expected class: `done/`→`terminal_done`, `rejected/`+`deferred/`→`terminal_closed`,
`pending/`+`accepted/`→`in_flight`. The gate:

- **FAIL (blocking):** a file in `pending/` or `accepted/` classified `terminal_done` —
  it claims completion while unfiled (the dangerous "shipped-but-unfiled" direction). The
  tree is **currently clean** of this, so the gate goes green on merge.
- **WARN (report-only, exit 0):** any other folder↔class mismatch — e.g. a `done/` file
  classified `in_flight` (cosmetic stale bullet; some exist today, fixed below), or a
  `pending/` file classified `terminal_closed`. `unknown`/no-bullet is a WARN, never a
  crash (fail-open).

**This PR re-runs the discovery and fixes whatever `done/` files carry a stale `in_flight`
State bullet** (≈4 at plan time) so the WARN report is empty, demonstrating the gate's
WARN→clean transition end-to-end. The PR description calls these cosmetic edits out
explicitly as bundled-for-demo so a wrong one-line edit can't be confused with gate logic.

Full "acceptance all-green ⇒ auto-file to done/" (the *active* reconcile that moves files)
needs the §1 runner to evaluate checks; it is **P5**, not here. P1 is detect-and-report +
the one safe blocking invariant.

### Check B — acceptance-block schema (§4)

Proposals have no YAML front matter (verified across the tree), so the `acceptance:` block
is embedded as a fenced block with an `acceptance` info-string:

````markdown
```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_foo"}
- {id: 5, tier: deployment, check: "ci:e2e-docker"}
```
````

The gate parses **every** such block in a file (`yaml.safe_load`; PyYAML is available in
the lint CI job — `check-api-conformance.py` already imports it under `make lint`) and
validates: the block is a list (empty list is valid — a draft may declare the block before
filling it); each entry is a mapping carrying **at least** the keys `id`/`tier`/`check`;
`id` is a positive int unique across all blocks in the file; `tier ∈ {mechanical,
integration, deployment, hardware}`; `check` a non-empty string. **Unknown keys are
tolerated** (forward-compat: a later packet may add `owner`/`timeout`/`schema_version`
without a breaking migration). A non-list, a missing required key, an unknown tier, a
duplicate/non-positive `id`, or a YAML parse error ⇒ **FAIL (blocking)**, with a
copy-pasteable skeleton + tier legend printed so the author can self-correct.

To make this check non-vacuous on day one (the roundtable flagged it would otherwise ship
with zero real input), **this PR adds a real `acceptance:` block to the substrate proposal
itself** — its mechanical criteria (build/lint/unit of this very gate) — so Check B
validates a genuine document at merge. P1 does **not** execute the checks (that is P3–P5);
it only validates their shape.

PyYAML import is guarded: an `ImportError` exits with a clear "PyYAML required for the
acceptance-block check" message rather than a traceback (it will not fire in CI, where
yaml is present, but keeps the script runnable/diagnosable elsewhere).

### Check C — premise-drift, report-only (§5)

For each `pending/`+`accepted/` proposal, extract high-confidence in-tree references and
report any that no longer resolve:

- backtick-quoted **repo paths** matching `src/…`, `scripts/…`, `docs/…`,
  `.github/…`, or a top-level `Makefile`/`Dockerfile*` — flag if the path does not exist;
- `check:` commands inside an acceptance block that name a `TEST=<id>` — flag if that test
  id greps nowhere under `src/tests/`.

**Report-only, never blocking, never auto-edits** (proposal Risk: "report-only … a
renamed-but-equivalent symbol" yields false positives). Bare-symbol/function references
are deliberately *out of scope* for P1 — too noisy to be a gate; only path/test-id forms,
which are precise, are reported. `--strict` promotes the report to a failure for callers
(e.g. a future driver) that want it fatal; `make lint` runs the **non-strict** form.
**Triage owner:** the drift report is consumed by the next plan revision / the autonomous
driver, not a human inbox; the script header records "first review at P3" and the condition
under which `--strict` becomes the lint default (when the drift count reaches zero and
stays there one packet). If Check C proves unworkably noisy it is removed, not endured.

### CLI / exit contract

`#!/usr/bin/env python3` (requires Python ≥ 3.8 — f-strings only, no walrus/match):

```
check-proposal-reconcile.py [--proposals-dir DIR] [--strict] [--json] [--plant-test]
```

- default: run A (blocking)+B (blocking)+C (report-only); exit 1 iff a *blocking* finding;
  print a human report. `--proposals-dir` defaults to `docs/proposals` resolved from the
  repo root (script-relative, like `check-proposal-links.py`).
- `--strict`: Check-C drift findings *also* make exit nonzero. Drift entries always live in
  the `drift` array regardless of `--strict`; `--strict` changes only the exit code, never
  where a finding is reported.
- `--json`: machine-readable `{blocking:[…], warnings:[…], drift:[…]}` — all three keys
  always present (possibly empty) — for the driver.
- `--plant-test`: inject one known-bad case per check class in-memory, assert each is
  caught, exit 0 on success / 1 if any planted fault slips through (proves non-vacuous,
  same convention as the other gates).

### Files

- **new** `scripts/check-proposal-reconcile.py` — the gate (named to match the established
  `scripts/check-*.py` lint-gate convention, not the broader `aimee dev reconcile` command
  that is a later packet).
- `src/Makefile` — add `proposal-reconcile-check:` target (`python3
  ../scripts/check-proposal-reconcile.py` then `--plant-test`), append to the `lint`
  aggregate + the `.PHONY` line, mirroring `proposal-links-check`.
- **edit (cosmetic)** the `done/` proposals carrying a stale `in_flight` State bullet
  (discovery re-run at impl time) → State bullet reads `done`, so Check A's WARN report is
  clean. Called out as bundled-for-demo in the PR body.
- **edit** `docs/proposals/pending/autonomous-dev-execution-substrate.md` — add the real
  `acceptance:` block (above) so Check B has a live input.
- **new** `scripts/tests/test_check_proposal_reconcile.py` — plain `unittest` over
  synthetic proposal trees in a tmpdir. Cases: Check A FAIL (pending claims done) / WARN
  (done has in_flight bullet, incl. an *unmodified* stale bullet) / clean / no-State-bullet
  (unknown→WARN, no crash) / rejected+deferred folders; Check B valid / missing-key /
  unknown-tier / dup-id / non-positive-id / non-list / empty-list (valid) / YAML parse
  error / two blocks per file / unknown-key-tolerated; Check C drift present (bad src path)
  / absent / `--strict` promotes drift to exit 1; `--json` shape with all three classes
  present simultaneously; `--plant-test` self-check. Self-contained (no network, no live
  tree).

### Verification (in-env, no human host)

1. `python3 scripts/reconcile-proposals.py --plant-test` → exit 0.
2. `python3 scripts/reconcile-proposals.py` on the real tree → exit 0, report lists the
   (now-fixed → empty) stale-state warnings and any path drift, no blocking findings.
3. `python3 -m unittest scripts/tests/test_reconcile_proposals.py` → all green.
4. `make -C src proposal-reconcile-check` → green; `make -C src lint` includes it.
5. `clang-format`/line checks N/A (no C changed); confirm `make -C src lint` stays green.

## Cross-cutting

- **No co-author trailers** (`no-coauthor-trailers` is a required check); branch off
  `origin/testing`, squash to `testing` with the `(#NNN)` convention; human-only promotion
  to `main`.
- **Per-packet loop:** implement → lint+tests green → roundtable review → fix to APPROVE →
  PR → green CI → squash-merge → next packet.
- **Scope discipline:** P1 ships *detection*. It never moves a proposal file or edits a
  proposal body beyond the 4 cosmetic State-bullet fixes. The file-moving reconcile is P5,
  gated on the §1 runner so "done" is evidence-backed, not guessed.
