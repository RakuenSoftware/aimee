# Proposal: Cross-repo dependency graph — recall recovery (build-declared edges)

- **State:** APPROVED — to implement (R1–R4). Roundtable design-reviewed to convergence over 5 rounds
  (blocking: 6 → 3 → 3 → 3 → 1, each finer; final round = a scope-honesty fix on §6, incorporated):
  separate evidence-tagged build-edge stream (not in cross_repo_route); canonical host/owner/repo URL
  mapping; evidence-tiered (URL/submodule MEDIUM-no-symbol, name-mapped annotate-only); enumerated
  gate applicability; precise `vendored`; explicit §2.6 merge table + §3.5 AMBIGUOUS routing; honest
  §6 split (build edges reach HIGH+MED ≥85%; HIGH ≥70% needs symbol corroboration, §7-limited).
  Follow-up to
  [cross-repo-precision-hardening](../done/cross-repo-precision-hardening.md), whose formal §9
  measurement (2026-06-28) passed precision (FP flood eliminated) but **collapsed recall to ~18%**
  (4 HIGH edges corpus-wide vs §5 gates HIGH ≥70% / HIGH+MED ≥85%).
- **Charter roles:** implementer (this agent) + roundtable reviewers (skeptic/correctness/perf).

## §0 Evidence (measured live, .254, 2026-06-28)
Full emitted edge set = **4** (gst-wayland-display→smithay, moonlight-qt→moonlight-common-c,
wolf→inputtino — true; inputtino→wolf — wrong-direction FP from indexed `.aimee/worktrees/` copies).
Recall ≈ 18%. Diagnosed blockers: (1) the real intra-corpus deps are **build-declared** (CMake
`FetchContent_Declare(... GIT_REPOSITORY .../<repo>.git)` / git submodules), fetched at build time, so
no source `#include` matches a definer repo → no `import_header` route → dropped by the H1 gate;
(2) some deps (C++ class/method APIs, e.g. mdns_cpp) capture no distinctive linking symbol at all, so
no symbol-model edge is possible; (3) indexing pollution (`.aimee/worktrees/` duplicate copies →
inputtino→wolf FP); (4) `cross_repo_identity` has no CMake identities (19 crate/npm/pypi only).

## §1 Thesis
A **declared build dependency on another corpus repo IS a dependency**, evidenced by the build graph
independent of symbol-level resolution. Build-declared edges are a **distinct evidence class** from
symbol-resolved edges — they must NOT be laundered through the symbol-graph route table (that would let
a declaration legitimize unrelated name-collision symbol matches). Carry an explicit `evidence_type`
so consumers can tell declaration-level adjacency from API-level usage.

## §2 Mechanism (blocking)

### §2.1 A separate build-edge stream (NOT via cross_repo_route)
Build-declared edges are computed in their own pass and **merged with the symbol-resolved edges only
at the resolver's output**. They do NOT write `cross_repo_route` and do NOT satisfy H1 for the symbol
path (so a build declaration can never turn an unrelated symbol/name collision into a HIGH edge —
roundtable blocker #1/#5). Each emitted edge carries `evidence_type ∈ {symbol_resolved, build_declared,
both}`, `build_kind ∈ {fetchcontent, submodule, link, manifest}`, `build_scope ∈ {production, test,
tool, optional}` where derivable, and **`parse_confidence ∈ {high, low}`** (R2 round-2 blocker): a
declaration whose mapping is fully resolved (literal `GIT_REPOSITORY` URL / `.gitmodules` entry /
Cargo-go.mod git+path, no unresolved `${VAR}`/generator-expr/conditional) is `high`; one resolved only
through a guessed `${VAR}` / conditional `if()` / generator expression is `low`.

### §2.2 Extraction (index-time, from file_contents) — top-level only
Per repo, parse ONLY the caller's **top-level** build/manifest files (its own `CMakeLists.txt` +
its explicit `include()` closure of tracked `.cmake` files; NOT `_deps/`/build-output trees — roundtable
transitive-attribution blocker): CMake `FetchContent_Declare`/`ExternalProject_Add` GIT_REPOSITORY,
`.gitmodules` submodule URLs, and (lower confidence) `target_link_libraries`/`find_package` target/
package names; Cargo.toml `[dependencies]`/`[dev-dependencies]` git+path, go.mod `require`,
package.json deps. Parser approach per ecosystem is regex-with-documented-limitations + a per-decl
parse-confidence; conditional `if()/foreach()`-guarded and `${VAR}`/generator-expression decls that
can't be resolved are tagged low-confidence (kept for recall but flagged, never promoted to HIGH).

### §2.3 Dep → corpus-repo mapping (canonical, not basename)
Map a git URL to `(host, owner, repo)` normalized (case-fold; collapse `-`/`_`; strip `.git`; resolve
relative/`${VAR}` where possible). Match to a corpus repo by its known remote `(host,owner,repo)` (or
repo-identity) → `parse_confidence=high`. **Basename-only matching is a last-resort fallback** (forks/
mirrors share basenames — blocker #2) → forces `parse_confidence=low` (so capped at LOW per §2.4); and
if the basename maps to **>1** corpus repo it is **AMBIGUOUS** (routed to review, not guessed) (round-3
blocker #1). A target/package NAME maps to a repo only via `cross_repo_identity` and only when it maps
to exactly one corpus repo (else AMBIGUOUS). Anything not mapping to a corpus repo (fmt/boost/external)
is dropped — corpus↔corpus only.

### §2.4 Tiering by evidence strength (roundtable blocker #3 + round-2 #1/#3)
- **URL / submodule / Cargo-go.mod git+path** with `parse_confidence=high` = ground-truth evidence →
  **MEDIUM** even with no linking symbols; **HIGH** (`evidence_type=both`) when also symbol-corroborated.
- **Any build decl with `parse_confidence=low`** (unresolved `${VAR}`/conditional/generator-expr) →
  capped at **LOW** (surfaced for review, excluded from the default MEDIUM+ output) — never MEDIUM/HIGH
  on a guess.
- **Name-mapped `target_link_libraries`/`find_package`** → NEVER a standalone edge. It only adds the
  `build_declared` flag (→ `evidence_type=both`) to an edge that **already exists as a fully
  symbol-resolved edge** — i.e. one that has already passed H1's route gate, §2 IDF, §5 kind, §3b
  header-IDF and prefer-local on the symbol path. Name mapping cannot create, re-tier, or weaken those
  gates; it is pure annotation of an independently-qualified edge.

### §2.6 Merge semantics + final tier table (AUTHORITATIVE — round-3/4 blockers)
The output is keyed by the **(caller, definer) repo pair**. For each pair the resolver may have a
symbol-resolved candidate (tier `S` ∈ {HIGH,MEDIUM,LOW}, from the H1–H7 path) and/or a build-declared
candidate. The single emitted edge per pair is decided by this table (this section is the sole tiering
authority; §2.4 only classifies the *build candidate's* strength, it does not set the final edge tier):

| symbol candidate | build candidate | → evidence_type | → final tier |
|---|---|---|---|
| S (any) | none | symbol_resolved | S |
| none | high-parse URL/submodule/git+path | build_declared | MEDIUM |
| none | low-parse | build_declared | LOW |
| S (any) | high-parse, **same definer** | both | **HIGH** (declared AND used = strongest) |
| S (any) | low-parse, same definer | both | S (low-parse never promotes; just annotates) |

`evidence_type=both` REQUIRES the symbol edge and build declaration name the **same definer repo**; a
symbol edge to a *different* repo never upgrades a build edge (they are distinct pairs). Build evidence
never *lowers* a symbol tier. Dedup is by (caller, definer); within a pair, multiple build declarations
collapse deterministically: highest `parse_confidence` first, then `build_kind` priority
(submodule > fetchcontent > manifest > link), then lexicographic by evidence string (round-4 blocker
#3) — the choice only affects the displayed `build_kind`/example, not the pair or tier.

### §2.5 Which gates apply (roundtable blocker #4 — enumerated)
Build edges **OBEY**: directionality (caller→definer only), corpus↔corpus pairing, untrusted-definer
§0 suppression, dedup, and AMBIGUOUS routing if a name maps to >1 repo. Build edges **BYPASS** (these
guard *name-collision* noise, irrelevant to a URL-declared dep): the H1 symbol-route requirement, §2
IDF/system-include distinctiveness, §5 kind eligibility, §3b header-IDF, §5 prefer-local. They are
NEVER written to `cross_repo_route`.

## §3 Precision guards (blocking)
- Corpus↔corpus only; directional; no reverse edges; URL/submodule evidence is exact-repo.
- **`vendored` defined precisely** (roundtable blocker #6): a file is vendored iff a path component
  matches a build-output/dependency-cache pattern (`_deps`, `build`, `cmake-build*`, `.git`,
  `.aimee/worktrees`, `node_modules`, `Pods`, `.venv`, `venv`, `site-packages`, `vendor`,
  `third_party`) AND the file is not in the caller's tracked top-level source. `originated_in_caller`
  ignores vendored definitions. This is correct for BOTH third-party vendored code AND a corpus repo
  fetched into the caller's `_deps/` (round-2 blocker #2): in either case the `_deps/` file is the
  DEP's code, not the caller's own definition, so it must not suppress the edge as "originated"; the
  symbol path then resolves the caller's *use* to the canonical (non-vendored) definer via H2
  canonical-preference. A vendored copy is never the sole definer of record.
- Indexing excludes `.aimee/worktrees/`, `.git/`, and build-output dirs (§5 R1) — also fixes
  inputtino→wolf.

## §3.5 AMBIGUOUS operational semantics (round-4 blocker)
A build declaration whose mapping is ambiguous (basename → >1 corpus repo; target/package name → >1
corpus repo) is written to the **existing `cross_repo_review` queue** (the same S4b adjudication path
symbol-AMBIGUOUS uses) with `evidence_type=build_declared` + the declaration as evidence — it is NOT
emitted as a default-output edge. For §6 acceptance it is counted in the Wilson **precision
denominator** (like symbol-AMBIGUOUS) and reported as a recall **coverage gap** (excluded from the
recall numerator). Unmapped declarations (external deps) are simply dropped, not surfaced.

## §4 Repo-identity completeness (suggestion)
Investigate why H0c CMake `project()`/`add_library` extraction yielded 0 CMake identities on the
corpus; populate them so target/package mapping (§2.3) and §5 recall measurement work.

## §5 Slices
- **R1 — indexing hygiene:** exclude `.aimee/worktrees/`, `.git/`, build-output dirs from the scan;
  re-scan; confirm inputtino→wolf FP gone. (cheap, self-contained)
- **R2 — build-declared edge stream (core):** index-time extraction (§2.2) + canonical mapping (§2.3)
  + the separate build-edge pass merged at output with `evidence_type`/`build_kind` (§2.1, §2.4);
  URL/submodule + Cargo/go.mod first. Pure pieces shim-tested; live re-measure.
- **R3 — originated-vendored exclusion (§3) + name-mapped link deps (symbol-corroborated only) + CMake
  identities (§4).**
- **R4 — re-measure §9** on .254 against a CURATED gold set (manual production/test/tool + direction
  classification, separate from the extractor — roundtable circular-validation suggestion), not just
  the extractor's own re-derivation.

## §6 Acceptance
§9 gates on the curated gold set, with an HONEST split by what each mechanism can reach (round-5
blocker): a build-declared-only dep is **MEDIUM** by design (§2.6), so build edges recover the
**HIGH+MED ≥85%** recall gate — that is the primary, reachable target of this proposal. The **HIGH ≥70%**
gate additionally requires symbol corroboration (`evidence_type=both`): reachable for deps with
capturable APIs (C-style functions, e.g. wolf→inputtino) but NOT for symbol-opaque deps (C++ class/
method APIs like mdns_cpp — §7 out of scope). So HIGH recall is **measured and reported**, expected to
improve materially (every C-style declared dep that is also used → HIGH) but its residual gap is
attributed to §7 symbol-extraction limits, not claimed as fully met by build edges alone. Precision:
Wilson-95% LB ≥90% with AMBIGUOUS in the denominator; the 4 existing true edges retained; no new FP
class; `evidence_type` exposed so consumers distinguish declared-adjacency from API-usage. Recall
reported separately for exact-URL/submodule vs name-mapped.

## §7 Out of scope
C++ class/method-level symbol extraction (mdns_cpp) — build-declared edges cover those deps at the
repo-pair level without it.
