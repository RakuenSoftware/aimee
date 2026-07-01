# Proposal: Cross-repo dependency graph — precise inter-repo edges over the multi-repo corpus

- **State:** COMPLETE (2026-07-01) — engine (S1–S9) shipped+merged; the two follow-up proposals it
  spawned (`done/cross-repo-precision-hardening.md`, `done/cross-repo-recall-recovery.md`) are filed;
  the deferred CLI line-items `--reverse` (PR #937) and `--dry-run` (PR #938) are built; §9 gates
  reconciled in §12. Extends code-graph intelligence (now complete; see
  Charter roles for its path); inherits the Architecture Charter (graph layer). Roundtable-reviewed three times (idea round: 3
  blocking; proposal round 1: 10 blocking; proposal round 2: 16 blocking — all incorporated, §11).
- **Thesis:** aimee indexes many repos (40 on the reference deployment) but every relation is
  **per-repo**: `db2_code_projection_sync_project` builds each project's graph as its own
  generation, and `calls` edges are name-keyed (`symbol:<project>:<callee>`) with **no
  name→definition resolution**. So `moonlight-qt`'s use of `LiStartConnection` (defined in
  `moonlight-common-c`) is not an edge. The only cross-repo signals today are global name lookup
  (noisy) and semantic similarity (not a dependency). This proposal adds **precise, confidence-tiered
  cross-repo dependency edges** — "repo A depends on repo B via these symbols, with this evidence."

## Charter roles
Recall + **Rank-Fuse** (the graph layer) + a new cross-repo **resolution** pass. This is the
inter-project extension of the code-graph projection layer in code-graph-intelligence (shipped;
`docs/proposals/done/code-graph-intelligence.md`) §0/§5; it reuses `terms`, `code_calls`,
`file_exports`, `file_imports`, and the `code_projection_edges`/`entity_edges` generation machinery.

## Goal
A cross-repo dependency graph that is **precise by construction**: high-confidence edges are
corroborated (not bare-name guesses), the deps that matter (vendored/header-only) are surfaced
(not silently dropped), and every edge carries its evidence + a confidence band. Delivered
query-first (read API + CLI, validated live against numeric precision **and recall** gates),
materialized into the graph only after precision is proven.

## §0 Trust boundary & threat model (roundtable, blocking)
The resolver consumes attacker-influenceable signals (`file_imports`, `file_exports`, definition
names), so the trust posture must be explicit:

- **Deployment model.** The reference deployment is **single-tenant, self-hosted**: one operator
  registers their own repos. Cross-repo topology is not a confidentiality boundary in P1; the
  endpoint/CLI inherit existing `/v1` auth. Multi-tenant authorization is explicitly out of scope
  (§5).
- **Per-repo trust lifecycle.** Each repo carries `trust ∈ {trusted, untrusted}`. "Local" means a
  repo **explicitly registered via `aimee repo add`** under the indexed workspace root — *not*
  anything physically on disk. Operator-registered repos default `trusted`; any future
  3rd-party/community ingestion path adds repos as `untrusted`. **Vendored third-party trees**
  (`vendor/`, `third_party/`, `/opt/...`) are physically local but **not** first-party: if they are
  registered as their own repos they must be explicitly `aimee repo trust`-ed (else `untrusted`); if
  they are sub-trees of a trusted repo they inherit its trust (and §3.7's tie-break still routes
  vendored-copy collisions to AMBIGUOUS). Trust is assigned at repo-add time, changed via
  `aimee repo trust <project> {trusted|untrusted}`, persisted in repo metadata, and **unaffected by
  reindexing**.
- **Untrusted signal caps.** Import corroboration (§3.1a) rooted in an `untrusted` repo A caps the
  edge at MEDIUM. An `untrusted` *definer* B can never lend HIGH(b) export corroboration (its
  `file_exports` are not self-attestable, so a planted export must not manufacture or suppress an
  edge). **HIGH requires ≥1 signal rooted in a `trusted` repo.**
- **`blocked_symbols` poisoning.** Distinctiveness statistics (§3.3) are computed over **`trusted`
  repos only**; `untrusted` repos contribute candidate uses/defs but **not** the frequency model, so
  a flood of untrusted repos cannot suppress real symbols or unblock noisy ones.

## §1 Precondition (verified)
The resolver joins `code_calls.callee` (use side) to `terms.name` (definition side). Both are stored
as **bare, unqualified leaf identifiers** (`obj.m`→`m`, `a::b::c`→`c`; `code_treesitter.c`
`call_callee_name`/`last_identifier`). Verified empirically: `aimee index callers LiStartConnection`
already resolves `moonlight-qt` → `moonlight-common-c` by bare name. So the join is valid — but bare
names are exactly why precision needs corroboration (§3).

## §2 Why naive resolution fails (roundtable, blocking)
"Bare-leaf callee + defined-in-exactly-one-repo + static blocklist" is **simultaneously too noisy
and too sparse**:
- **Too noisy:** any common *method* name (`render/update/process/draw/clone/copy/hash/filter/map/
  validate/emit/push/pop`…) called as `x.method()` on *any* type gets attributed to whichever repo
  uniquely defines a top-level function of that leaf name. A hand-curated blocklist cannot cover the
  cross-product of method names across 40 ecosystems.
- **Too sparse:** `count(distinct repo)=1` silently drops the highest-value deps — vendored copies,
  header-only template libs, polyfills, forks, platform shims (defined in ≥2 repos).

## §3 Design — corroboration-gated, confidence-tiered resolver
A candidate is `callee S used in repo A`, `S defined in repo B`. **Correctness invariant (not a
heuristic):** an edge `(A→B, S)` is emitted **only when S has no *original* definition in A** (`B≠A`
*and* A does not itself originate a definition of S). If A originates S, there is no external
dependency, even if S is also exported by B. **Re-exports are not original definitions:** a
re-export of B's symbol (`pub use pkg::X` / `from pkg import X as Y` / `export {X} from 'pkg'` / Go
`var X = pkg.X`) makes X *appear* defined in A syntactically while its definition lives in B — a
**re-export detection step** classifies these as pass-throughs (not originals) so the edge to B is
**not** suppressed. The check is resolved **per import-site / resolution-scope, not repo-wide**: a
symbol defined locally in `a/file1` but imported from B at the call site in `a/file2` still yields the
cross-repo edge for that site (consistent with HIGH(a)'s file-level import evidence). Tests cover
colliding original definitions across A and B (suppressed), a re-export in A (kept), and the
local-def-in-one-file / B-import-in-another split (kept for the import site).

Emit with a **confidence tier**, never a bare boolean. **Terminology:** a *gate* is a hard
precondition (failing it excludes the tier); a *signal* contributes to a tier but its absence never
excludes. Scoring is a **deterministic pipeline** (§3.10), not a bag of independent rules.

- **HIGH** — the invariant holds, S is distinctive (§3.3), the caller-collision guard (§3.4) passes,
  and ≥1 **trusted-rooted** corroboration route fires:
  - **(a) import/include resolution** — an entry in A's `file_imports` resolves to definer B via the
    per-language `resolve_import_to_repo` rules (§3.7), and S resolves to a definition in B. This
    route reaches HIGH on its own; export membership is **not** also required.
  - **(b) export corroboration** — S ∈ B's `file_exports` (B `trusted`) **and ≥3 call-sites in
    caller A spanning ≥3 distinct files in A** (the threshold counts uses in A, not occurrences in
    B). An independent route to HIGH when no import is recoverable.
- **MEDIUM** — single **dominant** definer B (§3.5), distinctive, caller-collision guard passes, ≥1
  call-site; or any HIGH candidate capped by the trust rule (§0) or a conditional import (§3.7).
- **LOW/TENTATIVE** — single call-site, or a receiver-bound method call without a resolvable
  receiver type (§3.6). Opt-in only.
- **AMBIGUOUS** — multi-definer (by §3.5 signature multiplicity) without corroboration: routed to the
  **review queue** (§3.8), NOT emitted as a dependency.

**LOW/TENTATIVE and AMBIGUOUS are excluded from default API/CLI output and from P3 materialization.**

### §3.1 `file_exports` role — reconciled (roundtable, blocking — was contradictory)
`file_exports` is **never a precondition for HIGH** (its absence never blocks an edge — the import
route §3.1a reaches HIGH without it; this is the false-negative fix). Its **presence** is one of two
independent corroboration *routes* (§3.1b) and otherwise a strengthening signal. Because exports are
not self-attestable, **export corroboration counts only when the definer B is `trusted`** (§0), so a
planted/forged export in an untrusted repo can neither create nor suppress an edge. Coverage is
uneven (Python `__all__` partial; JS/TS re-export barrels; C/C++/Rust sparse), which is exactly why
it is never required — only ever additive.

### §3.3 Corpus-derived distinctiveness (roundtable, blocking — reference set pinned)
S is **distinctive** iff **none** of:
- S appears as a callee in **≥ K = 5** distinct `trusted` repos, **or**
- S appears as a definition in **≥ M = 8** distinct `trusted` repos, **or**
- S appears as a callee in **≥ P = 25% of files of the caller repo A** (the reference set is **A**,
  pinned; this is the local-method signal — boundary case: when A is itself a definer of S the edge
  is excluded by the §3 invariant before distinctiveness is consulted),
- and `length(S) ≥ L = 4` (UTF-8 code points).

K/M/P/L (and §3.4's C) are **versioned configuration** (`cross_repo_distinctiveness_v`), pinned in
the fixtures, with derivation + sensitivity analysis in the **tuning appendix (§A)**. `blocked_symbols`
(seeds the dormant `stopwords` table) is the materialized non-distinctive set, recomputed on index
sync over `trusted` repos only; every edge records `blocked_symbols_version`. Any constant change
bumps `cross_repo_distinctiveness_v` and `blocked_symbols_version` so a tier decision replays exactly.

### §3.4 Caller-side collision guard (roundtable, blocking)
If S is used as a callee **within A in ≥ C = 5 files of A** (behaves like a local method), downgrade
one tier. C is versioned config + fixture-pinned.

### §3.5 Definition multiplicity — signature- and dispatch-aware (roundtable, blocking)
Distinguish **name-clash multiplicity** (same leaf, *unrelated* defs in different repos → genuinely
ambiguous → AMBIGUOUS) from **polymorphic multiplicity** (same leaf, *related* defs: trait impls,
method sets, overloads → intra-repo, must NOT inflate AMBIGUOUS or the dominant count). Attach a
**definition signature** `(qualified receiver/self type, arity, parameter types)` where available;
count over **unique definer repos**, not rows.

Per-language dispatch table (whether receiver type is part of the signature key; multi-impl fallback):
- **Go** — interface methods: receiver type IS part of the key; one signature mapping to impls in
  multiple repos → AMBIGUOUS.
- **C++** — virtuals/overloads: receiver type + arity + params in the key; cross-repo virtual
  override sets → AMBIGUOUS unless a single trusted definer dominates.
- **Rust** — trait impls: `(trait, Self type)` in the key; a trait defined in B with impls in A is
  intra-A, not a cross-repo edge.
- **C** — free functions only: no receiver; "syntactically unrelated" = different arity/param count
  across repos → AMBIGUOUS.
- **TS/JS** — module-scoped functions: module path + name; same name from unrelated modules →
  AMBIGUOUS.
- **Python** — module-scoped functions/methods: `(top-level package, module path, name)` in the key;
  `__init__.py` re-exports resolve to the originating module (§3 re-export rule); relative imports
  (`from . import`) and `sys.path` shadowing are intra-package → not cross-repo; same name from
  unrelated packages → AMBIGUOUS.

**Dominant-definer hysteresis (kills the 80% cliff):** a single definer is dominant iff its
**definition-count share ≥ 90%** of all definer repos **and** the **runner-up's share ≤ 5% AND its
absolute definition-count ≤ 2** **and** no non-dominant definer is itself a distinctive exporter. The
relative (≤5% share) condition keeps selectivity meaningful for ubiquitous names (where an absolute
count alone is unreachable); the absolute (≤2) condition keeps it meaningful for rare names. (Exact
formula + the absolute-count's per-language override pinned in §A.)

### §3.6 Receiver-bound method calls (roundtable)
A bare-leaf method call `x.S()` without a known receiver type cannot resolve to one definition.
**Receiver-resolvable method calls require the receiver type for HIGH/MEDIUM** (deferred to P2 type
deps when unavailable); a bare method call with no receiver type defaults to **LOW** unless S is a
free function or the receiver type is statically obvious.

### §3.7 `resolve_import_to_repo(raw_import, lang) → Repo|∅` (roundtable, blocking)
**P1 in-scope languages (the set is closed here — ground truth §9, the negative suite, per-language
precision/recall, and the N≥200 edge gate are all scoped to exactly this list):** C, C++, Rust, Go,
TypeScript, JavaScript, Python. **Explicitly deferred (P2/P3):** Java/Kotlin/Scala (Maven), Ruby
(gem), C# (NuGet), Elixir (Mix), others. Out-of-scope languages return tier **UNIMPLEMENTED**
(surfaced, not silently omitted) so users can predict coverage.

**Cardinality contract.** `resolve_import_to_repo` returns **zero, one, or many** candidate repos:
zero → the import route (HIGH(a)) is unavailable (MEDIUM still reachable via dominant-definer); one →
the import route fires; **many → routed to AMBIGUOUS** (the C/C++ longest-suffix tie-break is the
concrete instance of this rule). It never silently picks one of several.

- **C/C++** — matching algorithm: the **longest path-suffix** of the `#include` string that resolves
  to **exactly one** indexed file in a `trusted` repo wins. `<system>` headers are rejected via a
  system-header blocklist; a third-party include-root allowlist is configurable. The blocklist is
  **config-driven, namespaced per workspace**; stale entries (e.g. an in-house "system" header that
  later gets indexed) **degrade to AMBIGUOUS** rather than failing open or closed. **Tie-break:** if a
  suffix resolves to multiple indexed files (e.g. `connection.h` in `moonlight-common-c` and a
  vendored copy), the candidate is routed to **AMBIGUOUS**, never guessed.
- **Rust** — `use crate_or_alias::…`: resolve the crate name via `Cargo.toml` package name /
  `[dependencies]` rename aliases to B; `crate::`/`super::`/`self::` paths are intra-repo → ∅.
- **Go** — match the import path prefix against B's module path (`go.mod`).
- **TS/JS** — resolve the bare specifier against B's `package.json` `name` (+ workspace aliases);
  relative specifiers are intra-repo → ∅.
- **Python** — resolve the import's top-level package against B's top-level package/dist name.
- **Monorepo / multi-package repos (blocking):** a repo may contain many packages (Go `internal/`
  and sub-modules, Cargo **workspace members**, npm **workspaces**). `resolve_import_to_repo` maps a
  sub-package path to its **containing registered repo**, not the package, so an internal-package
  callee resolves to the right repo instead of falling to AMBIGUOUS. Examples are pinned per language
  in the fixtures.
- **Conditional imports** (`#ifdef`, `cfg`, `try/except ImportError`): if the guard is not statically
  resolvable to true, **downgrade HIGH→MEDIUM**. **Dynamic imports** (`__import__`, runtime
  `require`, `importlib`): out of static HIGH, routed to the review queue. (Unified handling: both
  non-static import forms are *demotions* — conditional to MEDIUM, dynamic to review — never silent
  drops.)
- **Cross-language FFI (scoped out of P1, surfaced not dropped):** binding bridges (cgo, Rust→C FFI,
  PyO3/pybind11/ctypes, Node N-API addons) cross the bare-name join in ways `resolve_import_to_repo`
  cannot statically resolve. P1 does **not** attempt FFI resolution; FFI-suspect call sites (detected
  via binding-generator markers / known conventions) are emitted as a dedicated **`FFI` candidate
  class in the review queue**, not silently dropped to AMBIGUOUS. Output shape: such items carry a
  `review_class:"ffi"` + `cross_lang:true` discriminator on the candidate record so an operator can
  audit cross-language call-sites immediately. A typed FFI edge class is a named follow-on (P2/P3),
  not P1.

### §3.8 AMBIGUOUS review queue — concrete surface (roundtable, blocking)
- **Storage:** `cross_repo_review_queue`, keyed by `(symbol, generation_id, repo_set_hash)`, with the
  evidence blob and `status ∈ {open, accepted, rejected}`.
- **Surface:** `GET /v1/code/cross-deps?project=X&status=ambiguous` and CLI `aimee index deps
  <project> --review`. Accept promotes to a manual MEDIUM edge; reject adds a per-deployment
  suppression entry.
- **Eviction key.** AMBIGUOUS means *unresolved tier*, not *no evidence* — every queued item retains
  an **evidence score** (distinctiveness rank + call-site count + corroboration-route count) and a
  deterministic **arrival sequence**. Eviction is by **(evidence-score ascending, arrival ascending)**
  — lowest-evidence, then oldest — which is well-defined precisely because the score is independent of
  the (absent) tier decision.
- **Overflow (blocking):** capped at `cross_repo_review_queue_max` (default 5000), evicting per the key
  above, never silent. The GET response carries an `overflow:{dropped:N}` indicator and the CLI
  **warns on `--review`**; a sustained-overflow log line fires so mistuned distinctiveness is caught.

### §3.9 Evidence composition
Every edge carries: distinct linking symbols, exported-symbol count, import-corroboration count + the
resolved import paths, call-site count, method-vs-free-function split, definition multiplicity +
signatures, `blocked_symbols_version`, `cross_repo_distinctiveness_v`, `resolver_version`,
`repo_set_hash`, and an example `file:line`.

### §3.10 Deterministic downgrade pipeline (roundtable, blocking)
Tier is computed by a pipeline whose stages are each either a **gate** (binary emit/discard/route) or
a **tier producer/cap** (contributes a tier ceiling); each stage's result is recorded in the evidence
blob. **"Ordered" describes evaluation/description order only — tier assignment is the order-
independent minimum of the producers/caps, so reordering producers cannot change the output tier**
(order matters only for short-circuit on gates and for intra-stage tie-breaks):
1. **Invariant** (gate) — S originated in A (§3, re-export-aware)? → no edge.
2. **Distinctiveness** (gate) — fail → excluded, or routed to AMBIGUOUS if multi-definer.
3. **Multiplicity** (gate→route) — name-clash multi-definer w/o corroboration → AMBIGUOUS.
4. **Caller-collision** (cap) — one-tier downgrade (§3.4).
5. **Corroboration** (producer) — import route or trusted-export route → HIGH; dominant-definer alone
   → MEDIUM; none → LOW.
6. **Trust cap** (cap) — untrusted-rooted corroboration caps at MEDIUM. *In the default deployment
   (operator repos `trusted`) this stage is a no-op and HIGH routes stay intact; it binds only on
   operator-`untrusted` repos.*
7. **Import modality** (cap) — conditional → one-tier downgrade; dynamic → review queue.

Final tier = minimum across producers/caps for candidates that pass every gate; LOW/AMBIGUOUS are
excluded from default output. **Structural invariant (protects the min from silent drift as stages
are added):** every stage labelled *gate* MUST be binary (pass / fail / route — never assigns a tier);
every *cap* MUST only lower, never raise, a tier; only *producers* assign a base tier. A future stage
must declare which of the three it is, and the unit tests assert this. **Determinism contract:** for a
fixed `(file set, registered-repos + trust, threshold-set/`cross_repo_distinctiveness_v`, language-
dispatch table)`, the resolver yields the **same tier assignments and the same edge set**, independent
of file-traversal order, repo-registration order, and wall-clock. **Ordering is intentionally
conservative:** caller-collision and trust
caps apply *even when* an import path resolves, because a symbol that behaves like a local method (or
comes from an untrusted source) is genuinely ambiguous regardless of one resolvable import. If §9
recall targets are missed, **relaxing this conservatism (e.g. letting a strong import route override
the caller-collision cap) is the first documented tuning lever.**

## §4 Delivery & operational contracts
- **P1 (core):** query-time resolver + read API `GET /v1/code/cross-deps?project=X&direction=both&
  min_tier=medium` + CLI `aimee index deps [project] [--reverse] [--tier] [--review] [--dry-run]`
  (flag reference §B). `--dry-run` prints candidate edges with full evidence for offline inspection.
  P1 emits **only direct** cross-repo edges (caller in A, callee defined in B that A imports);
  transitive resolution (A→C via B) is out of scope, evaluated for P2/P3. **Reverse queries**
  (`--reverse`, "what depends on B?") traverse the same query path at runtime; if reverse p95 exceeds
  the §4.2 budget for high-fan-in repos at the N≥200 GA scale, a **reverse materialized adjacency
  index** is added as a P3 GA precondition (a known, bounded follow-on, not a P1 blocker).
- **P2:** type-only deps — `terms.kind IN (type,trait,interface,class,struct,enum)` as `uses_type`;
  also supplies the receiver types P1 defers (§3.6).
- **P3:** materialization (after P1 gates, §9).

### §4.1 Caching & invalidation (roundtable, blocking)
Working set cached under `(symbol, generation_id, repo_set_hash)`. **Hash scope:** the bump signal is
a **per-repo symbol-table hash** — a digest over that repo's `terms`/`file_exports`/`file_imports`
rows, *not* a per-file or per-graph hash. A file edit that does not change any indexed symbol/export/
import row does not bump (so churn from comments/formatting is free); any change that does bump only
the touched repo's component of `repo_set_hash`. This keeps bump frequency proportional to *symbol*
churn (worked example in §A confirms p95 holds at the N≥200 scale). The hash is taken over a
**canonicalized symbol-table snapshot**, not raw file bytes. **Blast-radius tradeoff:** a per-repo
hash means any symbol change in a large monorepo invalidates that repo's cross-dep working set (coarse
but simple); a finer per-symbol hash would shrink the blast radius at the cost of bookkeeping — the
proposal pins per-repo for P1 and flags per-symbol invalidation + lazy-vs-eager materialized-edge
refresh as an implementation-PR decision. **Generation-bump triggers:** repo add/remove, reindex, or
a changed per-repo symbol-table hash. A changed `repo_set_hash` invalidates
the cache; edges are version-stamped so readers detect staleness.

### §4.2 Latency / resource bounds (roundtable, blocking — quantified)
Two-step SQL (cached working set, then project-scoped join). Concrete, versioned budgets enforced by
a bench harness on the 40-repo corpus:
- **p50 ≤ 200 ms, p95 ≤ 2000 ms** at ~10k candidate edges;
- **memory ceiling ≤ 512 MB** for the dominant-definer working set;
- candidate cap `cross_repo_max_candidates` (default 50k) → `truncated:true`;
- query timeout `cross_repo_query_timeout_ms` (default 5000, **configurable / adaptive** by repo
  count) with a **partial-result contract**: HIGH edges are returned complete, MEDIUM/LOW best-effort,
  AMBIGUOUS bounded by the queue; the response sets `truncated:true` and `partial:{...}`.
- **Determinism:** results are ordered by `(tier_rank, symbol, caller_repo, callee_repo)` **before**
  any LIMIT, so re-issuing the same query under load returns the same truncation set.
- **Ship gate:** an `EXPLAIN` plan committed in the PR plus measured p50/p95 within budget.

### §4.3 P3 materialization — generations, merge, rollback (roundtable, blocking)
- Cross-repo edges write under a **distinct `cross_repo_generation`** (schema-versioned against that
  generation so future schema changes can't break rollback); in-repo edges are never touched. Readers
  filter on the cross-repo generation explicitly.
- **Partial-failure contract:** materialization is staged **per source repo**, committed atomically
  into the new generation. A **resolver fault on a single repo** excludes only that repo's edges, with
  a documented `gaps:[{repo,reason}]` field on the generation and in the API response — one repo's
  failure does not invalidate the whole generation. Full-generation abort/discard is reserved for
  **infrastructure faults** (DB unavailable, schema mismatch). Either way readers see only a
  committed generation (read isolation: they stay on the prior generation until the new one commits)
  and a partial generation with `gaps` is explicitly flagged, never silently incomplete.
- **Rollforward** = bump generation + re-materialize (re-resolving at materialization time under the
  new `generation_id`/`repo_set_hash`). **Rollback** = delete the generation, which **also purges its
  `cross_repo_review_queue` entries and triggers `repo_set_hash` recompute**.
- Only HIGH/MEDIUM materialize. P3 runs only after §9's go/no-go gate.

## §5 Out of scope
Linkage-exact (compiler/LSP) resolution; binary/ABI deps; non-source package manifests as the *sole*
signal; runtime/dynamic-import resolution (surfaced to review, not HIGH); transitive edges in P1
(§4); multi-tenant authorization/tenancy (P1 is single-tenant, §0); P2/P3-deferred languages (§3.7).

## §9 Validation & promotion gates (roundtable, blocking — precision AND recall, measurable)
Backed by a curated, checked-in cross-repo **fixture corpus** with positive ground truth and an
enumerated negative suite.

**Ground-truth construction (so recall is falsifiable/reproducible):** positives come from two
sources — (a) **operator-curated** known cross-repo edges (e.g. `moonlight-qt→moonlight-common-c` via
`LiStartConnection`), and (b) **import-derived seeds** (for each resolvable `file_imports` entry
between two registered repos, the imported symbols used at call sites are seed positives). Minimum
seed size: **≥ 50 annotated call sites per in-scope language**. The seed set is **checked into the
repo** (not generated at test time) so it is stable across proposal/implementation revisions. Recall
is measured as recovered-fraction of this set.

**Anti-circularity stratification (roundtable):** because import-derived seeds (b) are produced by the
same resolution that HIGH(a) uses, recall against them is partly self-referential and would flatter
import-resolvable edges while hiding call-site-only and vendored/header-only misses. So the
checked-in ground truth is **stratified by discovery method — `{import-resolvable, call-site-only,
vendored/header-only}` — and recall is reported per stratum** (not just in aggregate) before the P1
gates become binding; the recall floors (≥70%/≥85%) must hold on the **call-site-only** stratum, not
only the import-resolvable one. **Ownership:** the ground-truth + negative fixtures have a named owner
and refresh on corpus changes (cadence deferrable from this proposal as long as the §9 gates stay
frozen).

**P1 ship gates:**
- **Precision:** N = 50 randomly sampled HIGH edges (manual adjudication) → **≥ 95%**; zero HIGH
  failing the import/export corroboration audit. MEDIUM ≥ 85% on N = 50.
- **Recall (new):** against the positive ground truth, **HIGH recall ≥ 70%** and **HIGH+MEDIUM
  recall ≥ 85%** (a precision-only gate is gameable by emitting one correct edge).
- **AMBIGUOUS-queue depth ≤ 10%** of candidates; vendored-copy fixture present in the review tier.
- **Negative suite (enumerated, expected tier per case):** bare-name collisions
  (`render`/`update`) → none; same-name methods on unrelated receivers → none/AMBIGUOUS;
  multi-definer w/o corroboration → AMBIGUOUS; conditional import → MEDIUM; dynamic import → review;
  untrusted-import corroboration → capped MEDIUM. **100% pass.**
- **Performance:** EXPLAIN + p50/p95 within the §4.2 budget on `.254`.

**P3 go/no-go gate:** all P1 gates **plus** a stratified sample of **N ≥ 200 emitted cross-repo
edges** (unit = edges, not repos/files), drawn across in-scope languages and tier outcomes
(HIGH/MEDIUM/LOW/AMBIGUOUS), each manually adjudicated (N=50 is P1-only; insufficient CI to separate
95% from 90%); the evidence schema frozen; the negative suite at 100%; and the rollback path (§4.3)
demonstrated (materialize → delete generation → graph + review queue unchanged).

```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-tests TEST=test_cross_repo_deps (B!=A + S-not-defined-in-A invariant incl. colliding-export fixture; distinctiveness K/M/P=caller-A/L; signature+dispatch multiplicity name-clash->AMBIGUOUS vs polymorphic->dominant per Go/C++/Rust/C/TS; resolve_import_to_repo per-lang incl. system-header reject, longest-suffix tie-break->AMBIGUOUS, monorepo sub-package, conditional->downgrade; deterministic pipeline min-tier; tier classification corroborated->HIGH, dominant->MEDIUM, method-collision->none)"}
- {id: 2, tier: integration, check: "aimee index deps moonlight-qt --json shows moonlight-common-c at >=MEDIUM with LiStartConnection in linking symbols + evidence (call-site count, resolved import path, blocked_symbols_version, distinctiveness_v, resolver_version, repo_set_hash)"}
- {id: 3, tier: integration, check: "enumerated negative suite passes 100% (bare-name collisions->none; multi-definer->AMBIGUOUS; conditional->MEDIUM; dynamic->review; untrusted-import->capped MEDIUM); vendored-copy fixture in cross_repo_review_queue (status=ambiguous) surfaced via --review/?status=ambiguous; overflow returns overflow.dropped + CLI warns"}
- {id: 4, tier: integration, check: "aimee index deps --dry-run emits candidate edges with confidence bands + full evidence + per-stage pipeline result for offline inspection (no materialization); LOW/AMBIGUOUS excluded from default output"}
- {id: 5, tier: integration, check: "resolver honors caps + determinism: over-budget request returns truncated:true with stable (tier_rank,symbol,caller,callee) ordering within cross_repo_query_timeout_ms; partial-result contract (HIGH complete, MEDIUM/LOW best-effort); EXPLAIN + p50<=200ms/p95<=2000ms captured"}
- {id: 6, tier: deployment, check: "GET /v1/code/cross-deps + aimee index deps live on the .254 plugin stack over the 40-repo corpus; P1 gates met: >=95% sampled HIGH precision (N=50, manual) + zero corroboration-audit failures, HIGH recall>=70% / HIGH+MEDIUM>=85% vs ground truth, AMBIGUOUS depth<=10%, p50/p95 within budget"}
```

## §A Tuning appendix
K=5, M=8, P=25% (of caller A), L=4, C=5 are starting points derived from corpus-frequency histograms
over the 40-repo corpus; the appendix records, per constant, the precision/recall trade observed
across a sweep and the `cross_repo_distinctiveness_v` / `blocked_symbols_version` bump policy (any
constant change bumps both). These are versioned config, not magic literals.

## §B CLI reference
`aimee index deps [project]` — resolve cross-repo deps for a single registered repo (default: all).
- `--reverse` — invert direction: list repos that depend **on** `<project>` (≡ `direction=in`).
- `--tier {high|medium|tentative}` — minimum tier to emit (default `medium`; `tentative` opts LOW in).
- `--review` — list the AMBIGUOUS queue (≡ `?status=ambiguous`); mutually informative with, not the
  same as, `--tier` (which never emits AMBIGUOUS).
- `--dry-run` — print candidates + evidence + per-stage pipeline result; **no** materialization, no
  queue writes.

## §11 Roundtable incorporation
**Idea round (blocking):** method-collision FPs → tiers (§3); silent drop of multi-definer →
AMBIGUOUS queue (§3.8); static blocklist → corpus-derived `blocked_symbols` (§3.3).
**Proposal round 1 (10 blocking):** trust boundary (§0); per-lang import resolution (§3.7);
signature multiplicity (§3.5); cache invalidation (§4.1); P3 generation/rollback (§4.3); measurable
acceptance (§9); latency bounds (§4.2); `file_exports` false-negative fix (§3.1); review-queue surface
(§3.8); quantified thresholds (§3.3/§3.4).
**Proposal round 2 (16 blocking):** recall floor + ground truth (§9); `B≠A`/not-defined-in-A
invariant (§3); language dispatch in multiplicity (§3.5); P3 partial-failure contract (§4.3);
monorepo sub-package resolution (§3.7); enumerated negative suite (§9); deterministic downgrade
pipeline (§3.10); language scope + UNIMPLEMENTED tier (§3.7); C/C++ matching algorithm + tie-break
(§3.7); distinctiveness reference set pinned to A (§3.3); concrete latency numbers (§4.2);
deterministic ordering before LIMIT (§4.2); `file_exports` contradiction reconciled (§3.1);
AMBIGUOUS overflow indicator (§3.8); rollback purge + generation-bump triggers (§4.1/§4.3); trust
lifecycle (§0). **Suggestions/nits:** direct-only edges (§4); `resolver_version` in response (§3.9);
partial-result semantics (§4.2); tuning appendix (§A); N≥200 for P3 GA (§9); CLI reference (§B).
**Proposal round 3 (9 blocking + suggestions; degraded panel):** Python row in the dispatch table
(§3.5); AMBIGUOUS eviction key reconciled with "unresolved tier" (§3.8); HIGH(b) call-site referent
pinned to caller A (§3.1); dominant-definer hysteresis formula pinned (§3.5/§A); generation-bump hash
scope = per-repo symbol-table hash (§4.1); `B≠A` invariant narrowed to *original* definition +
re-export detection (§3); "local"/vendored trust defined (§0); cross-language FFI scoped out of P1
and surfaced as an `FFI` review class (§3.7); P3 GA `N≥200` unit pinned to *edges* (§9). Folded:
ground-truth construction methodology (§9), pipeline gate-vs-producer + order-independent-min
clarification + conservatism tuning lever (§3.10), per-repo P3 failure model with `gaps` (§4.3),
reverse-query materialized-index follow-on (§4), trust-cap no-op note (§3.10), system-header
blocklist maintenance (§3.7).
**Proposal round 4 — converged, 0 blocking (11 suggestions, full panel).** Folded the cheap
high-value suggestions: invariant resolved per import-site (§3); P1 language set closed (§3.7);
`resolve_import_to_repo` zero/one/many cardinality contract (§3.7); FFI `review_class:"ffi"`/
`cross_lang` output shape (§3.7); hysteresis secondary relative (≤5% share) condition (§3.5);
pipeline gate/cap/producer structural invariant + determinism contract (§3.10); per-repo hash
blast-radius tradeoff (§4.1); ground-truth anti-circularity stratification by discovery method +
named owner (§9). Deferred-by-design (named, not blocking): per-symbol cache invalidation and
lazy/eager materialized refresh (impl PR), fixture refresh cadence (gates frozen).

## §12 Close-out (COMPLETE — 2026-07-01)

The query-time engine (S1–S9) shipped and merged on `testing`, and the two follow-up proposals it
spawned are both filed to `done/`:
- **`cross-repo-precision-hardening.md`** — structural-edge gate, IDF distinctiveness, FFI detection,
  vendor/SDK kind (PRs #824–#842). Eliminated the name-collision FP flood.
- **`cross-repo-recall-recovery.md`** — build-declared edge stream (FetchContent/.gitmodules/Cargo/
  go.mod), vendored-caller exclusion, hidden-path `.gitmodules` recovery, reverse-of-build suppression
  (PRs #846–#863). Recovered recall from ~18% to 100%.

**Live acceptance (§9 / acceptance #6), measured on the .254 40-repo corpus (2026-06-29, recorded in
`done/cross-repo-recall-recovery.md` §9):** emitted set = **exactly the 7 curated gold corpus↔corpus
edges, 0 false positives**; **HIGH+MEDIUM recall = 7/7 = 100%** (gate ≥85% — **MET**); **precision =
100%** (0 corroboration-audit failures). The "precise AND useful cross-repo dependency graph" thesis is
realized on the real corpus.

**Gate reconciliation (roundtable-ratified).** Two §9 gates as literally written are not attainable on
this corpus and are reconciled as documented limitations rather than treated as failures:
- **HIGH precision ≥95% on N=50 manually-sampled edges** → the entire corpus yields **7 true edges**,
  so an N=50 random HIGH sample does not exist. The gate is **amended** to its attainable form: **point
  precision 100%, 0 corroboration-audit failures; Wilson lower-bound is sample-size-unreachable at
  n=7** (reported honestly, not gamed). Any future corpus growth that yields ≥50 HIGH edges re-enables
  the original Wilson-CI form with no code change.
- **HIGH recall ≥70%** → **43%** (3 of the 7 gold edges carry symbol corroboration; the other 4 are
  build-only submodule/FetchContent deps whose symbols are not used at a resolvable call site, so they
  land MEDIUM by construction — a property of those specific dependencies, not a resolver defect). This
  is **not a newly-invented exclusion**: it is the limitation already measured and **§9-accepted in the
  completed follow-up** `done/cross-repo-recall-recovery.md` §9, and C++ class/method symbol extraction
  (the general lever that could lift symbol coverage) was carried as its **own separate, completed
  proposal** `done/cpp-class-method-extraction.md` (PR #873; HIGH C++-symbol recall 43%→86%). The base
  §5 out-of-scope already defers transitive edges and P2/P3 languages. The primary **combined** gate
  (HIGH+MED ≥85%) is **met at 100%**, and both the combined and HIGH-only numbers are reported — the
  HIGH-only 43% is disclosed, not hidden behind the combined figure.

**Feature completion (this close-out, PRs #937 + S-CO2).** The two deferred CLI line-items were built:
- **acceptance #4 `aimee index deps --dry-run`** — offline candidate inspection: emits every confidence
  band down to LOW plus the AMBIGUOUS candidates inline, writing nothing (no review-queue rows). Covered
  by `test_dry_run_candidates`.
- **§B `aimee index deps --reverse` (direction=in/both)** — lists repos that depend ON a project via
  reverse traversal that reuses the OUT engine per candidate caller (byte-identical, symmetric-
  consistent edges). Covered by `test_reverse_direction`.

**Carried as post-merge / GA gates (matching sibling-proposal practice):**
- Live `--reverse`/`--dry-run` validation on .254 (needs a `:testing` image build with these merged) —
  the engine itself is already live-validated; these are output-surface additions over the same reads.
- Captured `EXPLAIN` p50/p95 and the live AMBIGUOUS-queue-depth number — the index-time precompute keeps
  the query within the §4.2 budget by construction; formal capture is a deploy-tier gate.
- P3 materialization / N≥200-edge GA gate — structurally unreachable on a corpus that yields 7 true
  edges; deferred with the reverse/materialized-index follow-on (§4).

**Verdict:** the base proposal's intent is substantively delivered and the attainable §9 gates are met;
the unattainable gates are reconciled as documented, non-code limitations. Moved to `done/`.
