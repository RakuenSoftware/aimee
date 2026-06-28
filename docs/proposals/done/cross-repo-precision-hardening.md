# Proposal: Cross-repo dependency graph — precision hardening (structural edges, IDF distinctiveness, FFI-aware)

- **State:** IMPLEMENTED (13 PRs merged) + precision live-validated, but the FORMAL §9 acceptance is
  **NOT met** (recall). The formal Wilson-CI measurement (2026-06-28; docs/validation/
  cross-repo-precision-h4-runbook.md §"FORMAL §9 MEASUREMENT") shows the emitted edge set COLLAPSED to
  **4 HIGH edges corpus-wide**: §4 precision can't be sampled at the gate's N≥100 (observed 3/4 true),
  and §5 recall is **~18%** vs the gates (HIGH ≥70% / HIGH+MED ≥85%). The hardening eliminated P1's
  false-positive flood (good) but over-corrected — FetchContent/vendored deps, build/link-only deps
  (`target_link_libraries` with no source `#include`, not modelled by H0d), and a sparse repo-identity
  layer (no CMake identities) mean most real intra-corpus deps emit no edge. A **recall-recovery
  follow-up** is required before §9 passes (exclude `.aimee/worktrees` from indexing; build-link route
  extraction; CMake-identity population; FetchContent→canonical mapping — see the runbook). The
  precision MECHANISMS (§1–§6) are done + validated; the proposal's end goal (a precise AND *useful*
  graph) is not yet achieved at ~18% recall.
- **Implementation:** merged to `testing`, precision spot-check live-validated on `.254` (2026-06-28).
  Shipped across 13 PRs: H0a–H0d (#824/#825/#827/#828 metadata: def_kind, language, vendored,
  repo-identity, route index), H1 (#830 structural-edge gate + index-time rebuild), H2 (#831 vendor
  canonical-preference), H3a (#832 §5 kind eligibility + §4 vendored ceiling), H3b (#835 §2 header
  IDF; §2 angle-bracket + symbol-IDF found already-enforced), H5 (#838 prefer-local + generated-header
  reject), H6 (#840 angle-include capture + `is_system` — recall fix), H7 (#842 shared system-header
  list incl. Windows). Each slice roundtable-reviewed to convergence before merge.
  **Live re-validation (.254, full corpus re-scan, see docs/validation/cross-repo-precision-h4-runbook.md):**
  every known false positive collapsed — `moonlight-qt`/`wolf`/`aimee` → Sunshine (DEFINE_GUID,
  buffer_descriptor_t, process.h) all GONE (0 routes into Sunshine) — and the recall loss recovered:
  `moonlight-qt → moonlight-common-c` HIGH (via `<Limelight.h>`, previously dropped by the
  angle-bracket gap); true deps intact (`wolf → inputtino`). All of §1–§6 implemented or
  verified-already-satisfied. The formal Wilson-CI N≥100 precision/recall gate (§9) is left as a
  future measurement pass; this session's acceptance was a spot-check of the known FP classes (PASS).
  Original failure that motivated this: P1's ≥95% HIGH precision gate measured ~10–20% on the live
  40-repo corpus (2026-06-28).
- **Design history:** roundtable design-reviewed to convergence before implementation (round 1: 12
  blocking → the pivot to a structural-edge primitive, IDF distinctiveness, FFI-bridge detection,
  vendored canonical-preference, H0 metadata re-index; round 2: 6 blocking → repo-identity layer,
  per-ecosystem resolver, generated-output attribution, Tier-3 FFI, H0 index spec, recall join;
  round 3: converged, 0 findings; all incorporated, §12).
- **Thesis:** the P1 resolver emits an edge when a symbol is *used* in repo A and *defined in exactly
  one repo* B, gated by *name-frequency* distinctiveness and *name-suffix* import matching. The
  primitive is wrong: **a unique-name match is not a dependency.** On a heterogeneous real corpus,
  generic identifiers, vendored third-party SDK headers, and cross-language name collisions all
  satisfy "defined in exactly one repo" and produce confident HIGH edges that are not real. The fix
  is to make the primitive a **structural dependency edge** — the caller repo must contain an
  import/include/link/build route that *resolves into a file of* the definer repo, and the symbol's
  definition must live there — with name resolution only *disambiguating within* a proven structural
  edge. Distinctiveness becomes **data-driven (corpus IDF)**, cross-language is handled by **FFI
  bridge detection** (not blanket exclusion), and vendored trees by **canonical-preference** (not
  blanket exclusion).

## Charter roles
Same as the parent: Recall + Rank-Fuse (graph layer) + the cross-repo **resolution** pass. This
proposal modifies the resolution/classification stage
(`src/db2/cross_repo_resolver.c`, `src/db2/cross_repo_classify.c`,
`src/db2/cross_repo_stats.c`, `src/db2/cross_repo_deps.c`), adds index-time metadata to the kb
ingest/projection path, and replaces the frequency-only `blocked_symbols` model with corpus IDF; the
schema gains metadata columns + an inter-repo import-route index but the HTTP/CLI surface (S5–S7) and
review queue are unchanged.

## Goal
Raise live HIGH-tier precision on the reference 40-repo corpus to a **95 % CI lower-bound ≥ 90 %
(N ≥ 100, manual adjudication)** with **zero corroboration-audit failures**, **without** regressing
genuine positives below the parent's recall floor (HIGH ≥70 %, HIGH+MEDIUM ≥85 %) measured against a
**build-system-derived** ground-truth set. Re-run the checked-in acceptance harness
([`src/tests/test_cross_repo_acceptance.c`](../../src/tests/test_cross_repo_acceptance.c), extended by
S8) and the live runbook
([cross-repo-deps-acceptance](../../docs/validation/cross-repo-deps-acceptance.md)). This proposal
**gates** the parent's #5/#6 and P3.

## §0 Evidence — measured live failure modes (2026-06-28, `.254`, `:testing`)
Sampled HIGH edges via `aimee index deps <project> --tier high`. Of 9 HIGH edges, ~7 are false:

| edge | example symbol | failure class |
|---|---|---|
| moonlight-qt → Sunshine | `DEFINE_GUID`, `EGLDisplay`, `EGLSync` | vendored 3rd-party SDK headers (Win32/Khronos) indexed as definitions |
| Sunshine → aimee | `authenticate` | generic name, independently defined in both |
| aimee → Sunshine | `environment` | generic name |
| Sunshine → smithay | `motion` | cross-language collision (C++ caller, Rust definer) |
| inputtino → wolf | `create_touch_screen` | wrong direction (inputtino's own API) |

`blocked_symbols` (246 rows) only blocks names in ≥K=5 callee / ≥M=8 definer repos; generic/SDK names
present in 1–2 repos evade it. The defining repo is chosen by name-suffix matching that collides on
vendored header basenames, and corroboration never checks that caller and definer share a language or
a real include/link route. **Common root: no structural dependency edge is ever required.**

## §1 The primitive: a structural dependency edge is a hard prerequisite (blocking)
A cross-repo candidate is only eligible for **any** non-LOW tier if a **structural edge** A→B exists:
the caller repo A contains an import/include/link/build directive that **resolves into a file owned by
definer repo B**, and the symbol's definition lives in B. Name resolution only disambiguates *within*
a proven structural edge; a unique-name match with no structural edge is **LOW-unresolved** (excluded
from default output), full stop. Structural-edge sources, in strength order:

- **Source includes/imports** resolved to a file path under B (C/C++ `#include` resolved via include
  dirs; Rust `use`/`mod`+`Cargo.toml` path/git dep; Go import path↔`go.mod`; TS/JS module
  specifier↔`package.json`; Python import↔package). **Transitive** for source `#include` chains,
  umbrella headers (A.h→public/B.h→detail/C.h) and Python `__init__` re-exports — depth is
  config-driven (`kb.curator.cross_repo_graph.route_depth`, default 3), since a fixed shallow cap
  drops umbrella/nested-package edges.
- **Build/link routes:** CMake `find_package`/`target_link_libraries`/`FetchContent`, `Cargo.toml`
  deps, `package.json` deps, `go.mod` require, `pkg-config`. These also seed the recall ground truth
  (§4).
- **FFI bridges (§3)** count as structural edges for cross-language pairs.

The inter-repo import-route index that backs this is built at **index time** (H0), not per query.

### §1.5 Repo-identity layer + per-ecosystem resolver (blocking, in H0)
Structural resolution needs a canonical **identity → repo** map; without it a directive can't be
attributed to a repo. H0 builds, per repo, the identities it *provides*:

- **Manifest identities:** Cargo crate name, npm scope/name, PyPI module path, Go module path, CMake
  `project()`/target names + produced artifact (lib) names, pkg-config `.pc` name.
- **File identities:** the public header set (and their include-relative paths) and module file paths.

A directive is resolved to repo B by matching against B's provided identities. **Per-ecosystem
resolver + confidence band** (a directive that resolves *only* by a weaker method caps the edge):

| ecosystem | directive | resolved via | confidence |
|---|---|---|---|
| Rust/Go/npm | `use`/import/`require` | manifest dep ↔ provided module identity | HIGH-eligible (manifest is authoritative) |
| C/C++ | `#include "x"` / `<x>` | `compile_commands.json` include dirs → repo file; else public-header-set basename match | HIGH with compile_commands; MEDIUM on basename-only |
| Python | `import a.b` | provided module path (incl. `__init__` re-exports) | HIGH-eligible |
| pkg-config / CMake link | `.pc` / `target_link_libraries` | provided pkg-config / artifact name | HIGH-eligible (build route) |

`compile_commands.json` is opportunistic: present → precise C/C++ resolution; absent → basename
match against the provided public-header set, capped at MEDIUM (never the silent longest-suffix pick
of P1).

### §1.6 Build-time generated definitions (blocking, in H0)
Protobuf/gRPC/Thrift/FlatBuffers stubs, Qt `moc`/`uic`, CMake `configure_file`/`add_custom_command`
outputs, and `build.rs`-generated sources define symbols in files that don't exist at index time, so a
naive "definition lives in a file owned by B" check drops these real deps. H0 ingests **declared
generator outputs** from manifests (`add_custom_command OUTPUT`, `build.rs` declared paths, `.proto`/
`.thrift` compilation units, `configure_file` outputs) and attributes a generated definition to the
**generator's repo**. A symbol defined only in an un-ingestable generated file is LOW (coverage gap,
logged), never a false HIGH.

## §2 Distinctiveness is data-driven (IDF), not a static list (blocking)
Replace the frequency-threshold `blocked_symbols` with **corpus IDF over symbols *and* headers**: a
symbol or header basename defined/declared in ≥N distinct repos is non-distinctive (the multi-repo
vendored-copy and ubiquitous-name cases fall out automatically, no hand-maintained list). Add two
cheap structural signals that catch SDK identifiers regardless of corpus rarity:

- **Angle-bracket / system-include signal:** a symbol reached only through a `<...>` / system /
  toolchain-default include is an SDK identifier, not a first-party dependency (kills `DEFINE_GUID`,
  `EGL*` even when they appear in one repo).
- A **tiny seed blocklist** (a dozen unavoidable bare words like `main`/`init`/`type`) is permitted
  only as an IDF cold-start fallback, regenerated from a checked-in source — not the primary
  mechanism.

## §3 Cross-language: FFI-bridge detection, not blanket downgrade (blocking)
Do **not** blanket-downgrade cross-language edges (that collapses Rust↔C, PyO3/cffi, cgo, JNI, N-API).
Detect a **binding bridge** and let bridged cross-language edges reach MEDIUM/HIGH. Markers in tiers:

- **Tier 1 (source-level, tree-sitter-extractable, ships in H0):** `extern "C"` blocks,
  `#[no_mangle]`, JNI native-method signatures, `ctypes.CDLL`/`cffi.dlopen`/PyO3 `#[pyfunction]`.
- **Tier 2 (build-level):** `build.rs`/bindgen/cbindgen, cgo, SWIG, CMake link of a foreign target.
- **Tier 3 (runtime dynamic loaders):** `dlopen`/`dlsym`, `LoadLibrary`/`GetProcAddress`,
  `g_module_open`, `NSCreateObjectFileImageFromMemory`, etc. The literal library/symbol-name argument
  is joined against the §1.5 repo-identity layer (and any plugin-manifest metadata); a resolved
  dynamic load is a real (often plugin) dependency → **MEDIUM-with-evidence** (not HIGH — the binding
  is runtime, not statically verifiable).

Absent any bridge, a cross-language same-name pair is LOW (collision, e.g. C++ `motion` vs Rust
`motion`). Same-language remains the common case and needs no bridge.

## §4 Vendored trees: canonical-preference, not blanket exclusion (blocking)
A symbol may be **legitimately** vendored (CMake `FetchContent`→`subprojects/`; header-only `stb`/
`nlohmann` in `third_party/`). So instead of dropping every vendored definer:

- Classify every indexed file with vendored-path metadata at index time (H0).
- On a multi-candidate symbol, run a **canonical-preference** pass *before* raising AMBIGUOUS:
  (a) drop candidates that are vendored copies *when a non-vendored candidate exists*; (b) among the
  rest prefer the one reachable via the caller's structural edge; (c) only then, if still >1, route
  AMBIGUOUS. A **header-only-library exemption**: a vendored definer is still eligible when it is the
  *only* candidate and the caller has a structural include/build route to it.

## §5 Symbol-kind: SDK-style vs project-defined (blocking)
Carry accurate `terms.kind` (re-extracted in H0 — the live `DEFINE_GUID` mis-tag shows the current
kind data is unreliable). Macros/typedefs/vars are **not** auto-excluded (that drops opaque-handle
typedefs and function-like-macro public API). Instead: an SDK-style macro/typedef (caught by §2's
IDF + angle-bracket signal) is ineligible for HIGH; a **project-defined** typedef/macro/const reaches
MEDIUM/HIGH when it has a structural edge + clears IDF + (for HIGH) `file_exports` membership.

## §6 file_exports is a HIGH booster, not a MEDIUM gate (blocking)
Export membership is sparse (header-only libs, system SDKs, repos without doc-gen exports). MEDIUM
eligibility requires `(structural edge) AND (same-language OR FFI bridge) AND kind-eligible AND
IDF-distinctive`; **HIGH** additionally requires either import-route corroboration **or**
`file_exports` membership (export is a positive booster toward HIGH, never a floor under MEDIUM).

## §7 Evaluation pipeline order (suggestion, adopted)
Per-candidate, cheapest-rejecting-first: (0) **structural-edge gate** → (1) §4 vendor
canonical-preference → (2) §5 kind eligibility → (3) §2 IDF/system-include distinctiveness → (4)
language: same-language OR §3 FFI bridge → (5) AMBIGUOUS only after canonical-preference → (6) tier
assignment with §6 export booster. All inputs precomputed at index time (§11).

## §9 Acceptance criteria

```yaml acceptance
- {id: 1, tier: mechanical, check: "make unit-test-cross-repo-deps — structural-edge prerequisite (§1), IDF + system-include distinctiveness (§2), FFI-bridge detection (§3), vendor canonical-preference + header-only exemption (§4), SDK-vs-project kind (§5), export-booster tiering (§6) each unit-tested pure on the shim"}
- {id: 2, tier: mechanical, check: "make unit-test-cross-repo-acceptance — end-to-end: each prior live false positive (DEFINE_GUID, EGL*, authenticate, environment, motion cross-lang, create_touch_screen wrong-dir) yields NO HIGH/MEDIUM edge; a seeded FFI bridge (extern C) and a header-only vendored-but-canonical dep DO emit"}
- {id: 3, tier: integration, check: "H0 metadata present + non-null on the resolver hot path: per-file kind, vendored-path class, language, file_exports, and the inter-repo import-route index all populated for the 40-repo corpus"}
- {id: 4, tier: deployment, check: "live .254: N>=100 sampled HIGH edges manually adjudicated — Wilson 95% CI lower-bound >=90% true, zero corroboration-audit failures; AMBIGUOUS counted in the precision denominator"}
- {id: 5, tier: deployment, check: "live .254: recall vs build-system-derived ground truth — extract declared deps (Cargo.lock/package.json/go.mod/CMake target_link_libraries+find_package/pkg-config), map each declared dep package->repo via the H0 repo-identity layer, count a repo-pair recalled if the resolver emits any HIGH/MEDIUM edge for it; HIGH >=70% / HIGH+MEDIUM >=85%; AMBIGUOUS+LOW-unresolved excluded from the numerator but reported as coverage gaps; genuine edges (wolf->inputtino get_nodes, real FFI deps) retained"}
- {id: 6, tier: deployment, check: "live .254: negatives 0 across HIGH AND MEDIUM (>=5 generic-name + >=5 cross-lang non-bridge cases in the sample); AMBIGUOUS depth <=10%; p50<=200/p95<=2000ms with index-time precompute"}
```

## §10 Slices
- **H0 — metadata re-index + repo-identity + route index (blocking prerequisite).** Extend kb
  ingest/projection: accurate tree-sitter `kind` for macros/typedefs/vars; per-file vendored-path
  classification; per-file language; `file_exports` coverage; the **repo-identity layer** (§1.5);
  declared **generated-output** attribution (§1.6); Tier-1/2/3 FFI markers (§3); **and the inter-repo
  import-route index** (per ordered repo pair A→B: the strongest structural route + its confidence).
  **Storage:** db2/Postgres tables (consistent with the existing schema), populated at ingest/
  projection time — never per query. **Invalidation:** each repo carries a route-fingerprint =
  hash of (provided identities ∪ public-header set ∪ manifest contents ∪ INTERFACE include set);
  the route index for any pair touching a repo is rebuilt when that repo's fingerprint changes
  (same drift-guard pattern as `repo_set_hash`). Acceptance #3 verifies non-null on the hot path.
  This is the crux slice; it must land and be measured before H1's gate is meaningful.
- **H1 — structural-edge gate (§1) + Tier-1 FFI bridges (§3).** The primitive flip: no structural
  edge ⇒ LOW-unresolved. **Measure precision after H1** before proceeding.
- **H2 — vendor canonical-preference (§4) + kind eligibility (§5) + AMBIGUOUS canonical pass.**
- **H3 — IDF/system-include distinctiveness (§2) + export-booster tiering (§6).**
- **H4 — live re-validation (§4 of acceptance) on `.254`.** Recompute metadata, run the runbook,
  measure precision (Wilson CI) + recall (build-system ground truth), record numbers. On pass,
  unblocks the parent's #5/#6 and P3.

Each slice: worktree isolation, roundtable CODE review pre-PR, `make lint` + unit-tests green,
clang-format-19, no co-author trailers; PR → `testing`.

## §11 Performance
All new predicates (vendored class, language, kind, IDF/block status, `file_exports` set, and the
inter-repo import-route adjacency) are **precomputed at index time** and stored on the symbol/file
record or the route index — not evaluated per query — so the parent's p50≤200/p95≤2000 ms budget
holds. H4 captures `EXPLAIN (ANALYZE, BUFFERS)` to confirm. The route-index *precompute* itself
(transitive `#include` expansion on large C/C++ repos across the N² repo-pair space) is the cost to
watch; it runs off the query path during ingest/projection, is bounded by the per-repo
route-fingerprint (only changed repos recompute), and H0 records its wall-time so a regression is
visible.

## §12 Roundtable incorporation (design rounds)
**Round 1 (12 blocking):** structural-edge primitive as a hard prerequisite, not name-matching (§1);
IDF + system-include distinctiveness replacing the static-blocklist treadmill (§2); FFI-bridge
detection instead of blanket cross-language downgrade (§3); vendored canonical-preference + header-only
exemption instead of blanket exclusion (§4); SDK-vs-project kind instead of blanket macro/typedef drop
(§5); `file_exports` as a HIGH booster not a MEDIUM gate (§6); explicit pipeline order (§7); H0
metadata re-index as a blocking prerequisite (§10); Wilson-CI precision (N≥100) + build-system recall
+ AMBIGUOUS-in-denominator + negatives across HIGH&MEDIUM (§9); index-time precompute (§11).
**Round 2 (6 blocking, panel healthy/non-degraded — design accepted, specification deepened):**
repo-identity layer + per-ecosystem resolver with confidence bands (§1.5); build-time generated-output
attribution (§1.6); Tier-3 dynamic-loader FFI (§3); H0 index storage + route-fingerprint invalidation
(§10); recall ground-truth join via the repo-identity layer (§9 #5); configurable route depth for
umbrella headers / nested packages (§1); route-index precompute cost guard (§11).

## §13 Out of scope
P3 materialization (parent), `--reverse`/`--dry-run` (parent, deferred), multi-tenant authorization
(parent §5), and full semantic/type-signature resolution beyond the structural + IDF + kind + FFI
signals here (a possible P2 only if §1–§6 prove insufficient to reach the precision bar).
