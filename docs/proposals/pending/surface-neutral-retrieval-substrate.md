# Neutral rank-fusion reuse for web retrieval, plus a fail-closed egress capability gate

*Revision 3. Revision 1 proposed a broad surface-neutral retrieval substrate and
was rejected. Revision 2 withdrew that claim and narrowed scope; it was approved
with eight required changes. Revision 3 applies all eight. Full disposition is in
the Review record.*

## Executive decision

This proposal delivers two independent, separately revertible things:

1. **A fail-closed egress capability gate.** The workflow externalization gate is
   name-matched, and the native page reader is not on the list. Any egress-capable
   tool added after the list was written bypasses the gate.

2. **Neutral reuse of the existing pure rank-fusion primitive by the web
   retrieval adapter**, sequenced behind a contractual equivalence gate.

**Release order is fixed, not optional.** Slice 1 (the capability gate) lands
independently and before any production retrieval-adapter slice. The fixture-only
slice may land earlier if convenient, since it changes no production code.
Revision 2 said both "ships alone, first" and "may land in either order"; that
contradiction is resolved in favour of the security rationale.

This proposal explicitly **does not** propose a general retrieval substrate.
Generic emission, feature persistence, ranker fitting, retrieval events, and
caching are out of scope with named preconditions.

## What the evidence actually supports

Revision 1 claimed three duplicate implementations of one pipeline. That claim is
withdrawn. The accurate statement:

- `src/kb/kb_rrf.c` is **one** real fusion implementation: weighted RRF with
  deterministic tie-breaks, currently called from exactly one place,
  `src/kb/http/kb_http_code.c`. Its include list is limited to
  `math/stdio/stdlib/string`, which makes it dependency-light and free of DB and
  network coupling. That is an observation about dependencies, **not** a proof of
  semantic purity; any claim that it is side-effect-free must be established from
  its implementation behaviour, and Slice 4 does so before Slice 5 relies on it.
- `src/posix/web_read.c` combines a literal leg and a lexical leg by reserving a
  fixed fraction of an output byte budget (`WEBREAD_LIT_RESERVE 60`,
  `WEBREAD_BUDGET 1500`). This is **not** a second fusion implementation. It is a
  byte partition — a related question with different semantics.
- `src/server/session_search_tool.c` performs **no** ranking: `best_match_index`
  returns the first case-insensitive substring hit and falls back to message
  index 0 when nothing matches, so an unmatched query silently returns the
  session's first message as "the match". This is evidence of an *absent*
  capability, not a *duplicated* one.

The house rule — the third real duplication is the signal — is **not met for a
general substrate**, and this proposal does not claim it. What is supportable is
narrower: the web adapter is the natural *second caller* of an existing
primitive, which is reuse rather than abstraction.

## Part 1 — Fail-closed egress capability gate

### The defect

`src/modules/workflows/wfe_externalization.c` `DENY_EXACT` enumerates
`web_fetch`, `webfetch`, `fetch`, `http`, `http_request`, `curl`, `wget`.
`src/modules/workflows/wfe_native_gate.c` `name_is_web_tool` enumerates a similar
set. Grepping both for `web_read` returns nothing, while `src/posix/web_read.c`
performs real outbound network I/O.

The specific omission matters; the structural defect matters more. A name-matched
deny-list is **fail-open by construction** for every tool added after the list was
written. Adding one name fixes one tool and leaves the mechanism intact.

### Where the metadata lives, and who reads it

Egress capability becomes a **required field on the tool registration record**,
resolved at registration time. Both `wfe_externalization.c` and
`wfe_native_gate.c`, and every other invocation path that today consults a name
list, read the resolved metadata instead. The existing name lists are either
removed or retained only as **non-authoritative diagnostics** explicitly marked
as such — two policy sources that can diverge is the defect being fixed, not a
pattern to preserve.

### Coverage invariant: exact set, not count equality

Revision 2 proposed "the count of declared tools equals the count of registered
tools." That is insufficient, as the review noted: a missing declaration plus a
stale extra one yields equal counts. The invariant is exact key coverage:

- every canonical registered tool has **exactly one** valid declaration;
- no declaration refers to an unregistered canonical tool;
- every alias resolves to **exactly one** canonical tool;
- alias cycles, dangling aliases, duplicate names, and conflicting declarations
  are startup errors;
- every registration mechanism — including conditional and plugin registration,
  if present — passes through the same validation;
- authorization reads resolved metadata and never falls back to name matching.

### Lifecycle of a bad declaration

Omission, unknown enum values, and parse failures are **startup/registration
errors**: such a tool never becomes registered. Independently, the runtime
authorization path **also** denies any unknown value that reaches it despite
validation. This is defence in depth, not permission for malformed declarations
to persist.

Honest limit: mandatory declaration enforces *completeness*, and deny-on-unknown
enforces *conservatism*. Neither proves a human classified a given tool
correctly. The claim is enforceable completeness plus conservative unknown
handling — not proof that all egress behaviour has been identified.

### Tests

Explicit cases: `web_read` itself; an alias; an omitted declaration; an unknown
enum value; a dangling alias; duplicate and conflicting declarations. Each must
fail startup, and the authorization path must deny on unknown.

### Verification

Enumerate registered tool names and aliases from the tool table, not by grepping
the deny-list files. Revision 1's evidence was a grep, which is how the
`web_read` omission arose.

## Part 2 — Neutral rank-fusion reuse for web retrieval

### Identity: parent subject, candidate key

The ranked candidate is a **chunk**, not a page: `chunk_text` emits up to
`WEBREAD_MAX_CHUNKS 400` spans of ~`WEBREAD_CHUNK 480` bytes. Revision 1 used the
URL as the candidate key, which cannot address them.

- **Parent subject** — the page URL. Carried as out-of-band provenance, never as
  the fusion key. It is **not constrained by the 255-byte `kb_rrf_item_t.id`
  limit**; its only length bound is the `uint32` length field specified below.
- **Candidate key** — a digest over the length-delimited tuple
  `(parent_subject, chunk_ordinal)`, rendered to fit `kb_rrf_item_t.id`
  (`char[256]`, copied with `snprintf`, compared with `strcmp`).

Revision 2 required rejecting parent URLs longer than 255 bytes. That was wrong
and is withdrawn: the `kb_rrf_item_t.id` limit constrains the *rendered digest
key*, not the URL, so a long URL does not make the candidate key
unrepresentable. Rejecting it would be a user-visible regression unrelated to the
buffer limit.

Precisely stated, the design accepts any parent length representable by the
length field and the platform's hashing and allocation APIs, subject to resource
limits — not literally arbitrary length.

**Pre-image grammar.** The digest input is the byte concatenation:

```
  "aimee.webread.candidate.v1"   26 bytes, constant domain tag
  len(parent_subject)            uint32, big-endian
  parent_subject                 exactly that many bytes, no NUL terminator
  ordinal                        uint32, big-endian
```

A parent subject whose length exceeds `UINT32_MAX` is rejected. The encoding is
injective: the tag is a constant 26 bytes, the single variable-length field
(`parent_subject`) is preceded by its own fixed-width length, and `ordinal` is
itself fixed-width — so the pre-image parses back to exactly one
`(parent_subject, ordinal)` tuple, and no two distinct tuples can share one.

- **Algorithm**: SHA-256 over that pre-image, truncated to the **leading 128
  bits** (bytes 0..15 of the digest), rendered as 32 lowercase hex characters.
- **Rendered key**: `w:` + 32 hex + `:` + ordinal in unsigned decimal. Maximum
  length is 2 + 32 + 1 + 10 = 45 bytes, well inside `char[256]`.
- **Ordinal**: unsigned decimal, no padding. Values are bounded by
  `WEBREAD_MAX_CHUNKS`; a value at or beyond that cap is a programming error and
  aborts the operation rather than wrapping.
- **Embedded NULs**: covered by the digest, since the parent is hashed over its
  explicit length rather than as a C string. The rendered key is NUL-free by
  construction, being hex plus punctuation.
- **Collision scope**: these keys are **page-local**. They are constructed and
  compared only within a single `web_read` fusion invocation and are neither
  persisted nor compared across requests, so the collision domain is at most
  `WEBREAD_MAX_CHUNKS` (400) keys. At 128 bits that is far below any practical
  concern. The keys remain collision-*resistant*, which is probabilistic — the
  design does not claim impossibility. Should these keys later become
  cross-request or persisted identities, the collision domain grows to the total
  number of retained keys and this analysis must be redone.
- **Rejection** applies only to genuinely unrepresentable inputs: a parent length
  exceeding the length field, arithmetic or allocation failure, or failure to
  render the key — never to a parent merely because it is long, and never by
  truncation.

Widening `kb_rrf_item_t.id` is unnecessary and out of scope.

### Equivalence is a contract, not an inference

Revision 2 argued output is byte-identical "because the deciding code is the same
code." The review correctly rejected that: retaining the selector does not by
itself prove identity, because candidate production, ordering, and failure
semantics all sit upstream of it.

Byte identity is therefore a **stated contractual requirement of Stage A**,
enforced by a differential oracle:

- The **pre-refactor path is retained and runs alongside** the candidate-list
  path on the same inputs.
- The comparison covers **return status, emitted length, and every output byte**.
- Preservation is explicitly required for: leg ordering; duplicate-candidate
  behaviour; the `WEBREAD_MAX_CHUNKS` cap; tie ordering; query-needle buffer
  semantics (including queries longer than the 64-byte needle buffer); budget
  accounting at and across the boundary; and all failure behaviour.
- The old path is **kept available until the gate passes**, and only then removed.
- The harness must give each path **independently cloned or immutable inputs**,
  so the first execution cannot mutate shared input or state that the second then
  observes. A harness that lets the paths share mutable state would report
  identity it has not actually established.

Generated-input coverage, beyond golden fixtures, must include: a chunk appearing
in both legs; ties in lexical score; no literal match; empty query; overlong
query; chunk lengths straddling the budget boundary, including one that exactly
fills it; long parent URLs (asserting correct digesting, **not** rejection); and
more than `WEBREAD_MAX_CHUNKS` spans.

### Stage B — fusion as a measured, non-default alternative

Only after Stage A ships, `kb_rrf_fuse` becomes selectable. Governance:

- The retained byte-reservation selector **remains the default**.
- Stage B runs initially under **explicit opt-in or shadow evaluation** only.
- Changing the default is a **separately reviewed behaviour change** requiring
  defined relevance, output-stability, and security metrics. A fixture comparison
  alone does not authorize promotion.

The trust variant `kb_rrf_fuse_trust` is not used: the web adapter has no
earned-trust source.

## Out of scope, with preconditions

Each item was in Revision 1 and is removed. Per the review, "filed separately" is
an assertion until records exist, so the binding statement is: **each item below
must be filed as its own record, with the stated classification, before this
proposal is accepted for implementation.** None needs to be *implemented* here.

- **Search-result trust fencing (S3)** — *security, medium*.
  `web_search_format_results` (`src/server/web_search.c:429-465`) emits
  page-controlled titles and snippets with no trust marker, while `web_read.c`
  fences every span (`:377`, `:559`, `:615`). Revision 1 claimed a shared
  emission stage would fix this but never migrated the function, so the gap would
  have survived; an opt-in fencing flag would have made fencing defeatable
  besides. Fencing external content must be non-optional at the API level.
- **Search egress policy (S2)** — *security, medium*.
  `web_egress_addr_blocked` and `egress_resolve_validate` are called only from
  `web_read.c` and its test; `web_search.c` reaches the network via
  `agent_http_get` (`:268`, `:302`) and `http_retry_post` (`:375`) unvalidated.
  This is two policies, not one: a model-supplied page URL should be denied
  private/reserved destinations, whereas an operator-configured SearXNG endpoint
  may legitimately be on localhost or a private network. Correct handling reuses
  the *transport* behaviour — resolve once, validate, pin to the validated
  address, refuse redirects — since a validate-then-reconnect flow leaves a
  TOCTOU gap.
- **Session-history search defect** — *correctness, medium*.
  `best_match_index` returning message 0 on no match.
- **Result caching** — *enhancement*. Revision 1's key
  `(target_surface, subject_id, feature_set_version)` was wrong twice:
  `feature_rows` has **no** `target_surface` column — its columns are
  `subject_id, subject_kind, scope_kind, scope_id, feature_set_version,
  features, computed_at` — and a key omitting the query describes a *page* cache,
  not a *result* cache. Needs its own query-aware schema.
- **Learned or calibrated fusion weights** — *enhancement, blocked*.
  `src/kb/kb_ranker_fit.c:258-275` documents a `subject_space_mismatch` and a
  `missing_grouping_key`: "`feature_rows` has no retrieval_event_id/query column
  … so per-(query,candidate) training rows do not exist." Stage B supports
  **offline tuning of a single scalar against a labeled fixture corpus** —
  benchmark-driven hyperparameter search, which is neither the existing
  calibration stack nor learning to rank.
- **Generic chunk/leg/emission types** — *enhancement*. Precondition: a second
  adapter needing identical selection and emission behaviour.
- **String-keyed ranker/feature entry points and rank-fit surface
  parameterization** — *enhancement*. Not required by Stage A or Stage B.

## Slices

- **Slice 1 — egress capability gate.** Required declarations on the registration
  record; exact-set coverage invariant; alias integrity; startup errors for
  omission, unknown values, parse failures, cycles, dangling aliases, and
  conflicts; runtime deny-on-unknown; both gate call sites read resolved
  metadata; name lists removed or marked non-authoritative. **Lands first,
  independently.**
- **Slice 2 — web fixtures and differential harness.** Golden corpus plus the
  generated-input oracle. No production change; may land at any time.
- **Slice 3 — candidate identity and adapter conversion.** Digest-based candidate
  keys **and** expression of the two legs as ranked candidate lists, in one
  slice. Revision 2 split these across Slice 3 and Slice 4, which left candidates
  unmatched across legs in between; the review flagged it, and they are merged
  here. The existing selector is retained verbatim. Gate: differential oracle
  reports byte identity across all fixture and generated inputs.
- **Slice 4 — purity verification.** Establish from implementation behaviour —
  not from the include list — that `kb_rrf_fuse` carries no hidden state across
  calls, so Slice 5 can rely on it and so the differential harness can run both
  paths without cross-contamination. No relocation here.
- **Slice 5 — fusion as a measured alternative, then relocation.** Introduce the
  web adapter's call to `kb_rrf_fuse` behind an opt-in or shadow flag, default
  unchanged, differences reported as behaviour change. The neutral rename and
  relocation happen **in this slice, after the second caller actually exists** —
  Revision 3 placed the relocation in Slice 4, where the web adapter still had
  not called the primitive, so the "genuine second caller" justification did not
  yet hold. The house rule is satisfied only at this point.

## Release gates

- Slice 1's invariant tests fail if any registered tool lacks a declaration, if
  any alias is dangling or cyclic, or if any declaration is duplicated or refers
  to an unregistered tool.
- Slice 3's differential oracle reports byte identity — status, length, and all
  bytes — against the retained pre-refactor path on every fixture and generated
  input. The old path is removed only after this passes.
- No caller of `kb_ranker_rerank` or `kb_features_upsert` is touched.
- No database migration; `feature_rows` is unchanged.
- `web_read`'s egress posture is unchanged and re-verified: single resolution,
  deny-list validation, connection pinned to the validated address, redirects
  refused.
- Stage B's default remains the byte-reservation selector; promotion is a
  separate review.
- Sanitizers clean on all new tests.

## Review record

**Revision 1 — REJECTED.** Thirteen objections: the general-substrate claim was
unsupported by the third-duplication rule; the seam far exceeded the proven need;
byte identity was impossible under weighted RRF; page and chunk identity were
conflated; `char[256]` could not hold arbitrary URLs; `feature_rows` has no
surface column and the proposed cache key omitted the query; S3 was not actually
fixed and opt-in fencing was defeatable; S2 conflated untrusted URLs with
operator-configured endpoints; a capability boolean still failed open; the
"contract unchanged" claim contradicted a slice that tightened it; calibration
was overclaimed outside its disclaimer; security changes were bundled despite
claimed independence; and session search was absence, not duplication. All
accepted in Revision 2.

**Revision 2 — APPROVED WITH CHANGES.** Eight required changes and disposition:

| # | Required change | Disposition in Revision 3 |
|---|---|---|
| 1 | Byte identity is contractual, not inferred from retaining the selector; define a differential oracle against the pre-refactor path; keep the old path until the gate passes | **Applied.** Equivalence section rewritten; oracle compares status, length, and all bytes; enumerated preservation requirements; old path retained until gate passes. |
| 2 | Do not reject long URLs; digest the full length-delimited parent; specify algorithm, encoding, domain separation, key length, ordinal handling, NULs, collision semantics | **Applied.** Rejection-on-length withdrawn; full digest specification added; collision resistance stated as probabilistic. |
| 3 | Fix slice ordering around identity | **Applied.** Identity merged into the adapter-conversion slice. |
| 4 | Exact-set coverage invariant, not count equality | **Applied.** Six-part invariant plus enumerated test cases. |
| 5 | Precise lifecycle for unknown declarations; name metadata location; both gate sites consult it; remove or demote name lists | **Applied.** Startup errors plus runtime deny-on-unknown; metadata on the registration record; name lists removed or marked non-authoritative; enforceable-completeness limit stated. |
| 6 | Resolve "ships first" vs "either order" | **Applied.** Slice 1 lands first; fixtures may land earlier. |
| 7 | Substantiate "filed separately" | **Applied.** Reworded to a binding precondition with per-item classification. |
| 8 | Stage B explicitly non-default with promotion criteria | **Applied.** Default retained; opt-in/shadow only; promotion separately reviewed against defined metrics. |

Additional note accepted: "pure" for `kb_rrf` is stated as dependency-light and
DB/network-free, with semantic purity to be established in Slice 4 rather than
inferred from an include list.

**Revision 3 — APPROVABLE SUBJECT TO TWO COMPLETIONS.** The panel confirmed all
eight Revision 2 changes were genuinely applied in the body, accepted the
differential-oracle contract as a reasonable engineering standard, and confirmed
the capability gate is now fail-closed and the scope proportionate. Remaining
items, all applied in Revision 4:

| # | Required change | Disposition in Revision 4 |
|---|---|---|
| 1 | Complete the digest pre-image grammar: exact tag bytes, integer widths, ordinal encoding, truncation half | **Applied.** Explicit grammar with a 26-byte domain tag, `uint32` big-endian length prefixes, leading-128-bit truncation, and a proven-injective encoding. |
| 2 | Narrow the "arbitrarily long" claim | **Applied.** Restated as any length representable by the length field and platform APIs, subject to resource limits; `UINT32_MAX` rejection named. |
| 3 | Define the collision scope | **Applied.** Keys stated as page-local to one fusion invocation, domain bounded at 400; the analysis is explicitly flagged for redo if keys ever become persisted or cross-request. |
| 4 | Fix Slice 4/5 sequencing — relocation preceded the second caller | **Applied.** Slice 4 is purity verification only; relocation moved into Slice 5, after the web adapter actually calls the primitive. The evidence-section cross-reference was corrected to match. |

Additional caution accepted: the differential harness must supply independently
cloned or immutable inputs to each path.

**Revision 4 — NOT APPROVED, two wording defects.** The panel confirmed the
pre-image grammar is fully specified and injective, that Slice 4/5 sequencing is
correct, that the representable-length and page-local collision-scope statements
are accurate, and that the harness input-isolation requirement is present. Two
defects, both applied in Revision 5:

| # | Defect | Disposition in Revision 5 |
|---|---|---|
| 1 | "Not length-limited by this design" contradicted the `uint32` bound in the grammar | **Applied.** Restated as unconstrained by the 255-byte `kb_rrf_item_t.id` limit, bounded only by the `uint32` length field. |
| 2 | Injectivity rationale said "both variable-length fields are length-prefixed"; there is only one variable-length field, `ordinal` being fixed-width | **Applied.** Rationale rewritten to state the encoding parses back to exactly one tuple. |

**Revision 5 — APPROVED.** Both edits confirmed correct, with no new
contradiction introduced. The proposal is approved as an implementation plan,
subject to the standing precondition stated under "Out of scope": the S2, S3,
session-search, caching, and calibration records must be filed before
implementation is authorized.
