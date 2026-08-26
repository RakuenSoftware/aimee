# Proposal: front-end development module: runtime UI verification and design/visual QA

- **State:** PENDING. A module family built on pieces Aimee already has. One
  foundation extension plus three modules, ordered by dependency. Not started.
- **Review status:** CONVERGED. Roundtable approved rev 3 with zero findings
  (2026-07-23, 3 rounds: R1 6 blockers → R2 2 blockers → R3 approved).
  Rev 3 (roundtable round 2): verb enum pinned to exactly seven named members
  each with a tier; a `kind`-discriminated evidence chain adds a content-addressed
  `change` record so M3's before→change→after linkage is implementable and
  testable.
  Rev 2 (roundtable round 1): executor made per-run ephemeral with an explicit
  lifecycle + storage-wipe contract; egress mechanism committed
  (`--network none` boundary + proxy-over-socket + fail-closed launch flags);
  CDP passthrough forbidden; per-step evidence chain schema pinned to aimee-kb;
  tier→verb binding made an explicit Foundation deliverable and the safety
  wording de-circularized; source-read path scoped out of datamarking; cookie
  hygiene and fuzz posture added.
- **Author:** JBailes
- **Date:** 2026-07-23
- **Scope:** Give Aimee a first-party front-end *development* loop. Drive a
  running UI, review its design, verify behavior, and capture the result as
  tamper-evident evidence, for the apps Aimee itself builds and deploys. Not a
  general web-browsing/research capability (see *Non-goals*).
- **Charter roles:** Execute (drive a real browser against a running app),
  Persist (every navigation, action, and screenshot is a prev-hash-linked
  content-addressed record in aimee-kb; metrics carry numbers only), Review (a
  browser eats attacker-controlled bytes, isolation domain, per-run
  ephemerality, capability scope, and inbound-injection containment are
  load-bearing, not optional).

## What Aimee already has (and what it doesn't)

This proposal deliberately builds on existing pieces rather than reinventing
them.

- **The guard exists.** `src/server/computer_use.c` is a deterministic policy
  guard for external computer-use/browser MCP tools, wired into
  `agent_policy.c` and `agent_tools.c`: enable/disable, a 16-entry navigation
  **domain allowlist** (`computer_use_allowed_domains`), screenshot
  observe-vs-sensitive **approval**, sensitive-field (password) approval, and
  outbound sensitive-text **redaction**. The capability is disabled by default.
  It is **not** parameterized by capability tier today. The observe/interact
  verb binding below is new code, not an existing guarantee.
- **The egress model exists.** Delegates run `--network none` with a single
  unix socket to aimee-server, which holds all credentials and performs every
  external reach (`delegate-sandbox-aimee-sole-egress.md`); Aimee already has a
  hardened `web_read`/`web_search` egress policy.
- **The capability vocabulary remains externally owned.** The pending
  [`capability-scoped-agent-execution.md`](capability-scoped-agent-execution.md)
  describes an immutable Go invocation capability set and explicitly supplies neither posture tiers nor
  admission. This module must consume those decisions from the pending
  [`governance-policy-surface-and-posture.md`](governance-policy-surface-and-posture.md) owner.

What is **absent**, and is the value-add:

1. A **first-party browser executor**. Today the driver is an external MCP
   server Aimee only *guards*; there is no first-party, high-quality executor.
2. **Inbound-injection containment.** The guard redacts *outbound* data; nothing
   contains attacker page text steering the agent's reasoning.
3. **Tamper-evident runtime evidence.** The guard makes policy decisions; it
   does not commit a prev-hash-linked record of what the browser did.
4. **Capability-tier binding of the browser verbs**, and **the front-end
   development craft itself**, runtime UI verification, design review, and
   visual regression, which Aimee has nothing comparable to.

## Thesis

Aimee can already build and deploy front-end apps (`webchat-project-lifecycle`,
web-GUI clones pushing to aimee-kb). It cannot *see* whether what it built
actually works or looks right in a running browser. A unit test does not catch a
button rendered off-screen, a broken focus state, or an AI-slop layout. Closing
that loop. Build → **review the running UI** → **verify the behavior** → prove
it with evidence, is a well-scoped, high-value module that is squarely in the
"Aimee module" arena even though it is not Aimee's core mission.

The defensible core is narrow on purpose: **drive and verify the apps Aimee
itself builds and deploys.** That target is the only one Aimee can both *secure*
(it controls the deploy, so the destination is a genuine allowlist) and *needs*
to verify. It is also exactly where the front-end craft pays off.

## Foundation: extend `computer_use.c`, do not replace it

The guard is the policy spine. This module adds four things to it, each a
Foundation deliverable with its own test:

- **Accessibility-tree ref contract.** Actions address elements by sequential
  refs (`@e1`, `@e2`) derived from the accessibility tree and invalidated on
  navigation. A stale ref fails loudly rather than acting on the wrong element.
- **Capability-tier binding (new code; this is the safety spine, and it does
  not exist today).** The agent-facing surface is a closed enum of **exactly
  seven verbs**, each mapped to a minimum tier by a pinned table:
  - *observe* tier: `navigate` (allowlist-only), `snapshot` (accessibility tree
    → refs; META), `read` (fenced page content), `screenshot` (META), `back`.
  - *interact* tier: the observe set **plus** `click`, `fill`.

  There is no eighth or "implicit" verb, `snapshot` and `screenshot` are the
  META operations and are named members of the seven. The binding is enforced
  inside the existing `computer_use` guard path (`agent_policy.c`), compiles with
  an exhaustive switch over the seven-member enum (no default arm), and ships
  with a test that **fails closed if the binding is removed or a verb is added
  without a tier**. Acceptance #1 depends on this deliverable existing and being
  unit-tested. It is not inherited from the current guard.
- **Per-step evidence chain (pinned to aimee-kb invariants, not left to
  implementation).** A run is a single prev-hash-linked chain of records; each
  record carries a `kind` discriminator so verb steps and the M3 fix event share
  one chain:
  - `kind: "step"`: one per browser action:
    `{run_id, step_seq, kind, verb, url, ref, arg_digest, blob_hash,
    prev_step_hash, ts}`.
  - `kind: "change"`: one per applied fix (M3): `{run_id, step_seq, kind,
    commit_id, diff_manifest_hash, prev_step_hash, ts}`, where
    `diff_manifest_hash` content-addresses a blob listing the changed files and
    line ranges. The commit/diff itself is the worktree delegate's (M3); this
    record binds its identity into the chain.

  For every record `record_hash = SHA-256(canonical-pack of all fields including
  prev_step_hash)`, so the whole run, observe steps, the change, and the
  re-verify steps, is one genuinely chained, reconstructable, tamper-evident
  sequence under aimee-kb's existing chain rules. Screenshots/snapshots and diff
  manifests are content-addressed blobs (only the hash enters the record); blobs
  and records live in aimee-kb, never in metrics. Metrics carry counts only.

## Modules

### M1: first-party browser executor (the absent driver)

A real Chromium executor Aimee owns the **lifecycle** of, exposed to agents
through the existing computer-use tool surface and guard.

- **Per-run ephemeral lifecycle (not a long-lived service).** aimee-server
  spawns one executor instance per verification run, bound to that `run_id`,
  and destroys it at run end. Within a run, session state (cookies, tabs,
  localStorage) persists across commands so the loop is coherent; **across runs
  nothing survives**. A fresh instance with empty storage is created per run,
  and all cookies/localStorage/IndexedDB/service-workers are wiped at the run
  boundary. This is what makes "no more privileged than a delegate"
  architectural: the executor is as ephemeral and per-task as a delegate
  container, not a standing browser tied to a human session.
- **Isolation domain.** Chromium processes attacker-controlled bytes; a
  page-driven RCE must not reach Aimee's privileges or the vault. The executor
  runs in its **own hostile-isolation sandbox, no more privileged than a
  delegate**, lifecycle-managed by Aimee, never trust-domain-resident.
- **Network boundary is `--network none`, not a proxy.** The executor container
  has no network interface at all (identical to the delegate sandbox), so there
  is nothing for WebRTC, DoH, QUIC, DNS-prefetch, or a service-worker fetch to
  escape through. The kernel, not a proxy config, is the boundary. Legitimate
  fetches are *served* over the single unix socket to aimee-server, which acts
  as the forward proxy and enforces the `computer_use` domain allowlist
  server-side. Chromium is launched to route all fetch through that socket-bound
  proxy (`--proxy-server` at the socket bridge, `--proxy-bypass-list=` empty,
  DoH disabled, QUIC disabled, WebRTC IP handling forced to proxy/`disable_non_proxied_udp`).
  These flags are defense-in-depth; the `--network none` interface absence is
  the actual guarantee. A launch smoke test proves WebRTC, DoH, and QUIC all
  fail closed. This reuses the `delegate-sandbox-aimee-sole-egress` mechanism
  rather than inventing a parallel one.
- **Closed verb set, no CDP passthrough.** The agent surface is exactly the
  seven-member enum defined in the Foundation: navigate / snapshot / read /
  screenshot / click / fill / back. The executor speaks Chrome DevTools Protocol
  to Chromium internally, but **no agent-supplied string is forwarded into a CDP
  method**: the verb layer is an enum dispatch, and CDP methods outside the
  fixed set the seven verbs need, notably `Page.printToPDF`, full-page/clip
  `Page.captureScreenshot`, `Network.setBlockedURLs`, `Page.setBypassCSP`, and
  the file-chooser path, are unreachable from the agent. A test asserts that,
  for the exact seven-verb enum, no CDP method beyond the fixed per-verb set is
  reachable through the agent surface.

### M2: inbound-injection containment (gates M1's page content)

Page content is untrusted input. The lesson from the guard's own design. A
negative result means "no known pattern," never "provably safe", applies here:
**containment is architectural, not a classifier.**

- **Assume the agent will be injected.** The primary defense is that an injected
  observe-tier agent, held to the allowlist, with no vault access and no WRITE
  verb, has nothing dangerous to abuse. Capability scope is the boundary.
- **Deterministic datamarking.** `read`/`snapshot` output is fenced so the model
  treats it as data, with hidden/off-screen elements stripped rather than
  silently included. This is deterministic and is the real control.
- **Scope: page content only, source reads excluded.** Datamarking fences
  *page-derived* content. Source files the agent reads during a fix are
  Aimee-trusted inputs from the worktree, not page content, and are explicitly
  **out of datamarking scope**, fencing them would be miscategorization.
- **Persisted injection cannot cross runs.** Because storage is wiped at the
  run boundary (M1), attacker content stashed in localStorage/IndexedDB/service
  workers cannot survive into a later verification run. Within a run it remains
  fenced-as-data on every `snapshot`. The *task* (which defect to reproduce)
  comes from the operator/report, never from page content, so a fenced page
  cannot choose the agent's objective.
- **Classifier is telemetry, not a boundary.** An optional local injection
  classifier scores retrieved content and records suspected attempts as
  evidence (numbers only to metrics). It never gates safety and its absence
  never makes browsing "safe". It makes attempts *visible*.

M2 is a hard dependency: M1 page output does not reach the model without the
datamarking pass.

### M3: runtime UI verification and design/visual QA (the front-end craft)

The differentiated value, and the reason the module exists. A consumer of
M1+M2 that closes a **find → fix → verify** loop against an Aimee-built app:

- **Who applies the fix, and how it is admitted.** The fix is applied by the
  **same delegate that owns the worktree**, and the edit is admitted by the
  **existing attention-guard** exactly as any other delegate edit. M3 adds no
  new edit path and no new admission authority. The executor never edits source;
  it only drives the browser and produces evidence.
- **Runtime verification.** Reproduce the reported behavior through M1, let the
  worktree-owning delegate apply the scoped fix under the attention guard,
  re-drive to confirm, and capture a before/after screenshot pair plus the
  accessibility snapshot. The evidence chain **binds the fix to the change** via
  the `kind:"change"` record (Foundation): its `commit_id` and
  `diff_manifest_hash` sit in the same prev-hash-linked run chain between the
  before-state `step` records and the after-state `step` records, so "the agent
  says it's fixed" becomes an auditable, verifiable link from before-state →
  specific changed files/lines → after-state.
- **Design / visual review.** Score the running UI on concrete design
  dimensions (information architecture, interaction states, touch/click targets,
  accessibility, responsive behavior, AI-slop risk), edit source (via the
  worktree-owning delegate + attention guard) for mechanical fixes, and escalate
  genuine taste tradeoffs. Output is a design report backed by screenshots.
- **Visual regression.** Every fixed defect yields a captured baseline; a later
  run compares against it and flags drift. Baselines are evidence blobs, so a
  regression is provable, not asserted.

### Cookie / storage hygiene and audit (applies to M1 + M3)

Verification often runs against a logged-in dev surface. Therefore:

- Session cookies and all storage are **reset per run** (M1 lifecycle); no
  credential state carries between runs.
- The evidence chain records **URL / verb / ref / blob-hash only, never cookie
  or storage values**; sensitive-field and screenshot redaction from the
  existing guard still apply.
- The audit trail distinguishes an operator-initiated login ("I logged the agent
  into my dev app," an explicit `interact`-tier, approved step) from
  page-originated navigation, so an operator can tell "I authorized this session"
  from "a page tried to drive my session."

## Non-goals (v1)

- **No open-web interactive browsing / research, and the M1 executor is not a
  candidate for it.** Driving arbitrary hostile sites is where injection is
  maximal and where the destination cannot be a meaningful allowlist. The
  `web_read` path already covers read-only fetch under a hardened policy.
  Interactive open-web browsing is excluded for v1, and if it is ever added it
  must use a **separate executor posture**, reusing M1's executor against
  arbitrary sites is explicitly out of bounds, not merely "deferred."
- **No design-to-code generation.** Generating production HTML/CSS from mockups
  is a natural follow-on but is a separate module; this proposal is verification
  and review, not synthesis.
- **No codified replay/caching of browser interactions.** Speculative
  optimization; excluded until a measured need exists.

## Ordering

```
Foundation (extend computer_use.c: refs, tier→verb binding, per-step evidence chain)
        │
        ▼
   M1 (per-run ephemeral executor, hostile-isolation, --network none + socket proxy, no CDP passthrough)
        │  gated by
        ▼
   M2 (inbound-injection containment: capability + datamarking; storage wiped per run; classifier = telemetry)
        │
        ▼
   M3 (runtime verification + design/visual QA on Aimee-built apps; fixes via worktree delegate + attention guard)
```

Foundation + M1 + M2 is the minimum shippable unit (a governed, contained,
per-run-ephemeral, evidence-generating browser against allowlisted targets). M3
is the payoff.

## Acceptance

1. The Foundation's **tier→verb binding** over the exact **seven-verb enum**
   (navigate, snapshot, read, screenshot, back = observe; + click, fill =
   interact) exists and is unit-tested: an observe-tier agent is denied every
   WRITE verb (click, fill) with a fail-closed test over the exhaustive enum that
   breaks if the binding is removed or a new verb ships without a tier. (This is
   a new deliverable, not a property of the current guard.) The domain
   allowlist, enable flag, screenshot-approval, and sensitive-field/redaction
   behavior of `computer_use.c` are reused, not re-implemented.
2. The executor is **per-run ephemeral**: aimee-server spawns one instance per
   `run_id` and destroys it at run end; a test proves no cookie/localStorage/
   IndexedDB/service-worker state survives across runs. It runs in an isolation
   domain no more privileged than a delegate.
3. The executor container has **no network interface** (`--network none`); a test
   proves WebRTC, DoH, and QUIC fail closed and that every legitimate fetch
   traverses the aimee-server socket proxy + `computer_use` allowlist. No
   agent-supplied string reaches a CDP method; a test proves no CDP method beyond
   the fixed per-verb set of the seven-verb enum is reachable from the agent
   surface.
4. Every browser action is one `kind:"step"` aimee-kb append
   `{run_id, step_seq, kind, verb, url, ref, arg_digest, blob_hash,
   prev_step_hash, ts}`, and every applied fix is one `kind:"change"` append
   `{run_id, step_seq, kind, commit_id, diff_manifest_hash, prev_step_hash, ts}`;
   every record's hash includes `prev_step_hash`, so the run is a single genuine
   tamper-evident chain reconstructable end to end. Screenshots and diff
   manifests are content-addressed blobs; metrics carry counts only.
5. Stale accessibility refs fail loudly after navigation rather than acting on a
   re-numbered element.
6. `read`/`snapshot` page output never reaches the model without the datamarking
   pass; source-file reads are excluded from datamarking as Aimee-trusted;
   a planted injection page's directives are fenced as data, persisted-storage
   injection does not survive the per-run wipe, and any classifier hit is
   recorded as evidence, with the safety property resting on capability scope,
   demonstrated by an injected observe-tier agent being unable to perform any
   WRITE or off-allowlist action.
7. M3: a find→fix→verify run against an Aimee-built app produces a hashed
   before/after screenshot pair and snapshot; the **fix is applied by the
   worktree-owning delegate and admitted by the existing attention guard** (M3
   adds no edit path); the `kind:"change"` record binds `commit_id` +
   `diff_manifest_hash` into the run chain between the before and after `step`
   records, and a test **reconstructs and verifies the before→change→after
   linkage** from the chain (including resolving the diff manifest to changed
   files/lines); a design review emits a report backed by screenshots; a
   regression run flags drift against a stored baseline.
8. Open-web interactive browsing, design-to-code, and replay caching are absent
   from v1, and the executor exposes no posture that would enable open-web use.
9. Cookie/storage values never enter the evidence chain or metrics; the audit
   trail distinguishes an approved operator login from page-originated
   navigation.
10. Build, unit, integration, and sanitizer tests pass. Sanitizer/fuzz posture is
    named explicitly: ASAN + UBSAN on aimee-server-side code, **plus fuzz
    harnesses on the executor's URL/HTML/accessibility-snapshot/JS-bridge parser
    surface**, with the fuzz runs gating the release. Isolation-domain and
    egress-boundary tests are part of the release gate, not optional.
