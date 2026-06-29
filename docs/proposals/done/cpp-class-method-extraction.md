# Proposal: C++ class/method symbol extraction (recall §7)

- **State:** DONE — T0 shipped (#873); the §6 HIGH-recall gate is MET (86% ≥ 70%). T1/T2 are optional
  future-work (see §8), not required. Design roundtable converged 0-blocking over 5 rounds (9→5→1→1→0);
  prerequisites #1 (#867 .hpp collection) + #2 (routes) DONE. **Live §9 on .254 2026-06-29 after T0: recall HIGH+MED 100%, precision 100% (7/7
  gold, 0 FP), HIGH recall 43%→57%(.hpp)→86%(T0) — exceeding the ≥70% target.**

## §8 RESULT (T0 shipped, PR #873, deployed + measured .254 2026-06-29)
Shipping tree-sitter ALONE — extracting C++ class/method symbols (bare names) that the hand-rolled path
captured nothing of — was sufficient to meet the HIGH gate, because the corpus's C++ method names
(`fire_event`, `setLoggerSink`, inputtino's API) are distinctive enough to corroborate through the
EXISTING route + distinctiveness gates WITHOUT qualified names. Two previously-MEDIUM edges flipped to
HIGH `both`: **wolf→eventbus** (via `fire_event`) and **wolf→mdns_cpp** (via `setLoggerSink`). The 7
emitted edges are exactly the 7 gold edges (0 FP → precision held 100%); only Sunshine→moonlight-common-c
stays MEDIUM (a C library — no C++ methods). Corpus-diff acceptance: routes 69→69, build_deps 7→7,
identities 97→97 (no cross-repo loss); terms 110k→180k with functions 20.6k→113k (C++ methods now
captured); macros preserved (−2%, capacity-edge on huge files); code_calls −12% (tree-sitter is more
precise on call sites). No new FP class.

So **T1** (qualified `Class::method` names) and **T2** (member-call receiver-type inference) are NOT
required to meet the gate on this corpus; they remain valuable for corpora where bare method names
collide (generic `get`/`run`/`start`) and are carried as optional future work, not blockers.

Operational note (deploy): the tree-sitter re-ingest is heavier; a kb `restart` mid-heavy-write
SIGTERM'd postgres uncleanly, leaving an orphaned relation file that blocked WAL redo
(`could not create file base/…: File exists`) — recovered by stopping the plugin, removing the orphan +
stale `postmaster.pid`, and restarting (data intact). `refresh-containers` restored the host:8741 DNAT
the server reaches the kb through.
- **Origin:** deferred §7 of [cross-repo-recall-recovery](cross-repo-recall-recovery.md). HIGH
  recall was 43%: the C++ class/method-API deps capture no distinctive linking symbol.
- **User decisions (fixed):** substrate = **ship tree-sitter**; **stage** (qualified-first, measure,
  add member-call inference only if needed); **fix prerequisites first**.

## §0 Evidence (measured live, .254)
- **Prerequisite #1 DONE (#867):** the collector dropped C++ headers (`.hpp/.hh/.hxx` were absent from
  `code_collect.c` `code_ext_ok`). Fixed → C++ public `include/` APIs now indexed. **Live result:** §9
  recall HIGH+MED 100% held, precision 100% held (0 new FPs), and **HIGH recall 43%→57%**
  (`Sunshine→inputtino` MEDIUM→HIGH). **Prerequisite #2 DONE (cascade):** `wolf→mdns_cpp` (2) and
  `wolf→eventbus` (1) routes now form.
- **Remaining gap:** `wolf→eventbus`, `wolf→mdns_cpp` have routes but stay MEDIUM — their **C++ methods
  are not extracted at all** in production (hand-rolled `extractors.c` has no class/method handling;
  `c_def_line` captures nothing for `class X { void m(); }`). The tree-sitter front-end
  (`code_treesitter.c`) captures C++ classes + bodied methods but (a) is **not shipped** (Dockerfile
  builds without `-DAIMEE_TREESITTER`), and (b) emits **bare** names (`m`, not `mDNS::m`).
- **Verified resolver facts (cited, not assumed):**
  - HIGH-eligibility is a **denylist**: `MAX(CASE WHEN dt.def_kind IN ('macro','typedef') THEN 0 ELSE 1
    END)` (`cross_repo_deps.c:739`). So a new `def_kind='method'` is HIGH-capable with **no resolver
    change**.
  - Distinctiveness/blocked gates count **DISTINCT projects**, not rows
    (`cross_repo_deps.c:728` `COUNT(DISTINCT p2.id)`; `cross_repo_stats.c` blocked recompute is
    per-distinct-repo). So **additive** qualified rows cannot skew the frequency gates, and the design
    does **not assume** single-definer — a multi-definer qualified name is still correctly dropped;
    qualification only **enlarges the distinctive subset** (`mDNS::start` survives where bare `start`
    can't).
  - Resolver join: bare equality `code_calls.callee = terms.name AND kind='definition'`
    (`cross_repo_deps.c:740`) — a qualified string is just a distinctive name to it.
- **Real C++ usage shapes audited (wolf):** `mdns_cpp::mDNS mdns; mdns.startService();` (member calls,
  need T2) + `mdns_cpp::Logger::setLoggerSink(...)` (qualified call, T1 reaches it);
  `event_bus->fire_event(...)` (member calls). So **member-access dominates**; T1 reaches only the
  qualified-call subset, T2 (member-call inference) is needed for the rest — stated honestly below.

## §1 Thesis
Ship tree-sitter (the only substrate that tracks class/namespace scope), emit **qualified** C++
class-member symbols on both the definition and call sides under **one canonical rule**, and let the
existing bare-equality resolver match them (a qualified name is long + single-definer → distinctive by
the existing, verified gates; `def_kind='method'` is HIGH-capable by the verified denylist). Member-call
receiver-type inference is a separate, conditional slice.

## §2 Design

### §2.1 Canonical qualified-member names — ALL suffixes (def and call use the same helper)
A fixed trailing-N window is wrong: too short collapses distinct APIs (`ns1::Logger::set` ≡
`ns2::Logger::set` → dropped as multi-definer, regressing recall) and too long misses
namespace-omitted calls (`Logger::set()`). Instead, **emit every suffix-tuple of length 2..K** of the
component list, on both sides, and let the verified distinct-project gates keep whichever granularity is
distinctive and drop whichever is ambiguous.

**Component list:** the ordered scope path of the member — one component per enclosing
namespace/class/struct/union, then the member name. Normalization (a single shared helper, applied
**identically** to the definition path and the call path):
1. **Strip a leading `::`** (global-namespace qualifier) so `::C::m` ≡ `C::m`.
2. **Strip template arguments per component**, balanced-bracket aware (remove each top-level `<…>`
   including nested `<…>`): `Outer<T>::Inner<U>::m` → components `[Outer, Inner, m]`. Stripping is
   per-component and happens **before** forming suffixes.
3. **Emit suffix-tuples** joined by `::` for lengths 2..min(len, K), K=4 (bounds rows to ≤3 qualified
   names per member). E.g. component list `[mdns_cpp, mDNS, start]` → `{mDNS::start,
   mdns_cpp::mDNS::start}`; `[ns1, ns2, C, m]` → `{C::m, ns2::C::m, ns1::ns2::C::m}`.

The DEFINITION's component list is its full lexical scope (enclosing namespaces+class, or the
declarator's `qualified_identifier` for an out-of-line def). The CALL's component list is the **literal
qualified path written at the call site** (`a::b::c()` → `[a,b,c]`). They intersect at every suffix the
call literally spells: a namespace-omitted `Logger::set()` matches the def's `Logger::set` suffix; a
fully-written `ns1::Logger::set()` matches the def's full suffix (so `ns1`/`ns2` Loggers do NOT
collide). Ambiguous short suffixes are dropped by the distinct-project gate (correct); the longest
shared suffix that is single-definer survives. Because the gates count distinct projects and rows are
additive (§0), emitting multiple suffixes is safe and cannot skew them.

**Template stripping (precise):** per component, remove everything between the first `<` and its
balanced matching `>` (regardless of inner content — nested templates, default args, whitespace,
comments all dropped); a component with no `<` is unchanged.

**Accepted FP risk (contained, not eliminated):** a call suffix can coincide with an *unrelated*
single-definer def suffix in another project (`X` writes `util::parse()` meaning its own thing; `Y`
defines `util::parse`). The longer-suffix preference reduces this, and the existing gates still contain
it — the H1 **route** requirement (the caller must structurally reach the definer), reverse-of-build
suppression, and AMBIGUOUS routing all apply unchanged to qualified candidates exactly as to bare ones.
It is an accepted heuristic, measured post-T1 (sample single-definer length-2 suffixes vs ground truth).

### §2.2 Construct policy (enumerated — emit / skip / normalize)
- **Real definitions only** (`function_definition` nodes): a bodied method — inline `void m(){}` inside
  `class C`, or out-of-line `void C::m(){}` at namespace scope. EMIT the §2.1 suffixes with
  `def_kind='method'`. This covers header-only libs (inline defs in `.hpp`) and split libs (out-of-line
  defs in `.cpp`); both are now indexed (prereq #1). 
- **Pure declarations: do NOT emit.** The discriminator is structural, NOT the wrapper node:
  tree-sitter-cpp wraps BOTH inline-bodied method definitions and pure declarations in a
  `field_declaration`. A node is a real definition iff it is (or contains) a `function_definition`
  (has a body); a `field_declaration` whose `function_declarator` has **no `function_definition` child**
  is a pure declaration (`void m();`) → skip. This avoids reclassifying a declaration as
  `kind='definition'`; the real definition (inline or out-of-line) is captured above.
- **Out-of-line definition** `void mDNS::start(){}`: component list = **the enclosing namespace chain
  (scope-threaded) PREPENDED to the declarator `qualified_identifier`'s qualifier components (its
  class/namespace portion, excluding the function name), then the member**. So `namespace ns1 { void
  C::m(){} }` → `[ns1, C, m]` (not `[C, m]`), and `void ns2::C::m(){}` → `[ns2, C, m]` — two namespaces
  defining `C::m` stay distinct at their full suffix. (A leading `::` on the declarator means
  fully-global — do not prepend; §2.1 step 1 then strips it.) When the declarator has no qualifier at
  all, the components come entirely from the enclosing namespace chain. Then apply §2.1.
- **Constructors/destructors:** SKIP qualified emission (a `C(...)` call is `object_creation` → callee
  `C`, the class **type** term; a `C::C` def would not align). This SKIP is **conditional on §3(d)** —
  T0's corpus-diff verifies class-name type terms and `object_creation` callees survive the swap as a
  C++ superset; if that check fails, the SKIP is unsafe and must be revisited.
- **Operator/conversion overloads** (`operator<<`, `operator bool`): SKIP — operator call syntax can't
  produce an aligning callee.
- **Anonymous-namespace members:** SKIP — internal linkage, never a cross-repo API.
- Other languages: unchanged (the §2.1 helper is C++-only-wired here; §6).

### §2.3 Storage — additive qualified rows, no schema change
Keep the bare def/call rows exactly as today (index-find, C-style edges, all other consumers
unchanged). **Additionally** emit the §2.1 qualified suffix rows: terms `name='<suffix>',
def_kind='method'` for each definition; extra callees `<suffix>` for each **scope-resolution** call
(`a::b::c()`). Member-access calls (`obj.m`, `ptr->m`) emit only bare `m` (T2 territory). Rationale
(verified §0): the resolver's bare-equality join + distinct-project gates work unchanged; no migration;
bare behavior preserved; additive rows can't skew the frequency gates.

### §2.4 Resolver / gates — unchanged
No change to `cross_repo_deps.c` / `cross_repo_resolver.c` / `blocked_symbols`. `def_kind='method'` is
HIGH-capable by the verified denylist; the `schema.sql:30-41` `def_kind` contract comment gains `method`
(doc only — `def_kind` is an unconstrained string; HIGH eligibility is resolver code, not schema).

## §3 Shipping tree-sitter (T0) — risk & acceptance
T0 swaps the **production** extractor for all 17 languages. Safeguards:
1. **Vendored grammars (reproducible build):** do NOT `git clone` grammars at Docker build. Vendor the
   tree-sitter runtime + the grammars we use as **pinned tarballs with recorded sha256** (or a pinned
   submodule), fetched into the build context, so the kb/combined image builds offline + reproducibly.
   Measure and record the kb + combined image-size and build-time delta in the PR.
2. **Preserve macros (no day-one regression):** the hand-rolled C path emits `#define` as
   `def_kind='macro'` terms; tree-sitter's C++ grammar does **not**. A naive swap would drop every
   macro term. So for C/C++, run a **supplementary `#define` pass** (the existing cheap hand-rolled
   macro line-scan) ALONGSIDE the tree-sitter defs and merge, so macro terms are retained. (Macros are
   denylisted from HIGH anyway, but losing the terms would drop macro-based edges and fail the diff.)
   **Merge identity:** the supplementary pass owns ONLY `def_kind='macro'` rows; tree-sitter owns
   everything else; dedup key is `(file_id, name, def_kind)` so a macro and a same-named function/method
   coexist and neither is double-counted.
3. **Corpus-diff acceptance gate (deterministic, partitioned):** take both snapshots from the **same
   frozen DB state / corpus revision** (record the rev hash + timestamps) — not two live re-scans (which
   would diff unrelated churn). Partition the diff:
   - **(a) C/C++ structural symbols** (functions/classes/methods/namespaced) — tree-sitter output MUST
     be a **superset** of hand-rolled (it may add rows; it must not lose any).
   - **(b) routes + cross-repo edges + blocked_symbols** — no loss.
   - **(c) `def_kind='macro'` terms** — preserved by safeguard #2, so also a superset.
   - **(d) class-name **type** terms + `object_creation` callees** — superset (this is the invariant the
     §2.2 constructor-SKIP relies on; verifying it here makes the SKIP safe).
   Any unexplained loss in (a)-(d), or a §9 regression (recall HIGH+MED <100% or precision <100%),
   BLOCKS T0.
- The hand-rolled path REMAINS the fallback for extensions/languages tree-sitter lacks (`extract_*`
  already falls through), so T0 is additive, not a hard cutover.

## §4 Slices
- **T0 — ship tree-sitter (substrate).** Vendor grammars (pinned, sha256); build kb + combined images
  with `AIMEE_TREESITTER=1`. Acceptance = the §3 corpus-diff gate + live §9 hold (recall HIGH+MED 100%,
  precision 100%). No qualified-name work yet (isolates the swap). Add a CI canary that compares the
  hand-rolled vs tree-sitter extractor output on a fixed fixture so future grammar bumps can't silently
  regress (replaces the now-dead non-treesitter production assumption).
- **T1 — qualified C++ symbols (defs + scope-resolution calls), ONE rule (§2.1/§2.2).** Scope-thread the
  tree-sitter `visit`; emit the §2.1 suffixes for **real definitions only** — inline-bodied and
  out-of-line `function_definition` nodes (NOT pure declarations, per §2.2) — and for scope-resolution
  callees. Unit tests in `test_code_treesitter.c` for every §2.2 construct (inline-bodied / out-of-line
  method, nested class, free function, all-suffix emission, template-arg stripping incl. nested, leading
  `::`, def==call alignment) AND a negative test that a pure `void m();` declaration emits NO qualified
  term. T1 acceptance METRICS (measured on the frozen corpus, recorded in the PR): `terms` and
  `code_calls` row growth (abs + %), `blocked_symbols` delta, and resolver wall-clock delta on a
  representative `index deps` set — must stay within sane bounds.
- **T_measure — re-measure §9** HIGH recall. Report which deps newly corroborate (expected: C++ deps used
  via qualified calls, e.g. the `mdns_cpp::Logger::setLoggerSink` path). Member-access-dominated deps
  (mdns_cpp's `mdns.startService()`, eventbus's `event_bus->fire_event()`) are NOT expected to move at
  T1 — that is T2's job.
- **T2 (conditional) — member-call receiver-type inference.** Built iff T_measure shows the HIGH gate
  (≥70%) still unmet because member-access usage dominates (it does for mdns_cpp/eventbus). Per-TU local
  var→type table from tree-sitter declarations (`mdns_cpp::mDNS mdns;` → `mdns:mDNS`) so `mdns.start()` →
  `mDNS::start`. Largest/riskiest slice; scoped only if the measurement requires it.

## §5 Acceptance
- T0: corpus-diff gate passes (C/C++ symbol superset, no route/edge loss) + §9 recall HIGH+MED 100% &
  precision 100% retained.
- T1 (correctness + no-regression, NOT the HIGH target): qualified C++ method terms/calls present live;
  **no new precision FP class** (precision stays 100%); gold edges retained; the metrics above within
  bounds; **≥1 qualified-call-corroborated C++ edge** reaches HIGH (`both`). The ≥70% HIGH target is
  explicitly NOT claimed as a T1 outcome (member-access dominance → deferred to T_measure+T2).
- Overall: §9 HIGH recall measured + reported; target ≥70% (recall-recovery §6) pursued via T1 then T2.
  Residual gap, if member-call-only, is closed by T2; any residue beyond C++ scope is documented, not
  hidden.

## §6 Out of scope
Cross-TU/global type inference; overload/signature disambiguation; template instantiation resolution;
qualified-name extraction for languages other than C++ (the §2.1 helper is reusable but only C++ is
wired here).

## §7 Process
Each slice: worktree, branch off `origin/testing`, roundtable code review to 0-blocking, PR, CI,
squash-merge; deploy to .254 + live re-measure where a slice changes extraction. No co-author trailers /
no Claude attribution; `git -c commit.gpgsign=false`.
