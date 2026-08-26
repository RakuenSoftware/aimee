# What aimee should take from a local web-search pipeline, and what it shouldn't

> **Archived proposal.** This records the design as it was agreed, not the
> system as it behaves today; parts of it have since diverged. For current
> behaviour see `docs/`, or the code.

- **State:** DONE. Delivered scope archived 2026-07-26.

> **Archived complete (2026-07-26).** The audit found the scoped deliverables shipped,
> superseded by the current implementation, or fully represented by completed child slices.

*Capability-by-capability assessment of an external local-web-search project
against aimee's existing systems. Every verdict names the aimee file it lands
in, or says "greenfield". Numbers come from measurement, not from the other
project's README.*

## How this was assessed

Three sources, in decreasing order of authority:

1. **Real aimee traffic.** 304 agent session transcripts, 35,555 tool calls, of
   which 205 are web calls (104 page reads, 101 searches). 94 unique
   (url, prompt) pairs; 84 pages fetched for measurement. This is the only
   evidence about what aimee's web tools are actually asked to do.
2. **Their source**, read directly (~3.5k lines of Python).
3. **Their published evals**, read for methodology.

Their benchmark harness is sound and is not the basis for any criticism here:
it runs a real frontier model per arm, grades with an LLM judge on the SimpleQA
protocol including a `NOT_ATTEMPTED` class, uses a **fresh** cache so every
search is charged ("a worst case for us"), and charges its own engine fees at
published rates. Their headline cost/token claims were not reproduced and are
not disputed.

**The one thing to keep in mind throughout:** they solve *open-web question
answering*, unknown pages, unknown wording, vocabulary mismatch between question
and document. aimee's web tools mostly serve *"read this page I already chose."*
Capabilities transfer well when the problems line up and badly when they don't,
and that distinction decides most of the verdicts below.

## aimee's web surface today (verified)

| | |
|---|---|
| `src/server/web_search.c` | DDG HTML scrape / SearXNG / Tavily. Returns title+url+snippet as an 8 KB text block. **No cache, no fanout, no result dedup, no egress validation.** |
| `src/posix/web_read.c` | Fetch one page, SSRF-guarded (single resolve, IP-pinned, redirects refused), strip to text, extract query-relevant windows under a 1500-byte budget, fenced untrusted. |
| `src/db1` | Local SQLite. Already the store for the search handle→URL map. |
| `src/server/http_retry.c` | Retry/backoff for HTTP. |
| `src/server/token_tracker*.c` | `token_estimate_cost`, per-model pricing. |
| embedder | `Dockerfile.embedder`, `kb/kb_service_code_embed.c` exist and are **not wired to the web path**. |

## Ranked findings

### 1. Fuse page extraction into search: the single highest-value item

**What they do.** `pipeline.py` runs search → concurrent fetch → extract → rank
→ compress as *one* operation. The caller gets relevant page content, never
engine snippets.

**What aimee does.** `web_search` returns snippets; the agent must then decide a
URL and make a second `web_read` call. Engine snippets are ~150 chars of
marketing text chosen by the engine, not by the query.

**Why this is the top item.** `web_read` is now a clean extraction primitive
that takes (page text, query, budget). Nothing about it is specific to being
called directly. Calling it over the top N search results is a small change that
turns search from "here are some links" into "here is the relevant text", and
it is where their token advantage actually comes from, not from chunking or
fusion, but from never putting a whole page in context.

**Prerequisite, non-negotiable:** `web_search` has **no egress validation**.
`web_egress_addr_blocked` / `egress_resolve_validate` are called only from
`web_read.c`. Fetching N attacker-influenced result URLs through an unguarded
path would be a straightforward SSRF. See
[search-egress-policy-split.md](search-egress-policy-split.md), which stops
being an enhancement and becomes a blocker.

**Cost to be honest about:** fetching N pages per search turns a ~1s call into
several seconds. They report 10–40s for fresh searches. Needs a bounded fan-out,
a deadline, and partial-result tolerance.

**Verdict: ADOPT.** Medium effort, gated on the egress split.

### 2. Result caching: ADOPT

**Theirs.** `cache.py`: two SQLite layers, page text by URL and ranked results by
query key, lazy TTL expiry.

**aimee.** No retrieval cache anywhere (`semcache|semantic_cache|retrieval_cache`
returns nothing). `tool_web_read` refetches on every call, including repeated
reads of the same page inside one investigation.

DB1 is already the local SQLite store. The design work is the key, not the
storage: it must include the query, the budget, and a policy version, or it is a
page cache pretending to be a result cache. See
[retrieval-result-cache-schema.md](retrieval-result-cache-schema.md).

**Verdict: ADOPT.** Small-to-medium. Highest quality-of-life win after item 1.

### 3. Multi-engine fanout with URL-keyed dedup: ADOPT

**Theirs.** `search/multi.py`: concurrent engine calls via `ThreadPoolExecutor`,
results fused by RRF **keyed on a normalised URL** (`_normalize_url`), so the
same page from three engines counts once and ranks higher.

**aimee.** Single backend per call. No dedup, no cross-engine ranking.

The valuable half is **URL normalisation and dedup**, which is deterministic and
needs no model. The RRF over engine ranks is a genuine use of rank fusion,
unlike the intra-page case, these are independent ranked lists of the
same items. `src/kb/kb_rrf.c` already implements weighted RRF with deterministic
tie-breaks and would fit here without modification.

**Verdict: ADOPT.** This is the one place rank fusion genuinely belongs.

### 4. Engine circuit-breaking and failover: ADAPT

**Theirs.** `search/resilience.py`: consecutive-failure breaker with a cooldown
bench and a half-open probe; treats **empty results as failure**, which is right
for scrapers that return 200-with-nothing when they have decided you are a bot.

**aimee.** `http_retry.c` handles retries but has no per-engine health state, so
a dead engine is retried on every call.

**Verdict: ADAPT.** Small. Take the empty-as-failure insight and the cooldown;
build it on `http_retry.c` rather than porting their class.

### 5. Provenance and cache-age on results: ADOPT

Cheap, and it is what makes a cache safe to trust: say where a span came from and
how old it is. `web_read` already cites by ordinal and fences as untrusted;
adding source URL and fetch age is a small extension. Do it **with** item 2, not
after.

**Verdict: ADOPT.** Small.

### 6. Savings accounting: ADAPT, and only report facts

`receipts.py` reports cost avoided. aimee already has `token_estimate_cost` and
`cost_shaped_reward` in `token_tracker*.c`, so the arithmetic exists. Report
observed cache hits, bytes and tokens returned, not a counterfactual "what
hosted search would have cost", which is unfalsifiable.

**Verdict: ADAPT.** Small, after the cache exists.

### 7. Extraction cascade (readability → rendering): PARTIAL

**Theirs.** `fetch/html.py` cascades trafilatura → readability → newspaper4k →
Playwright, to survive JS-rendered pages and 403 walls.

**aimee.** `html_to_text` strips tags and collapses whitespace. Adequate for
docs and READMEs (which is 47 of 94 real fetches (GitHub + raw.githubusercontent))
and poor on JS-heavy pages.

Take the *idea* of a fallback ladder if real fetches start failing. Do **not**
take Playwright: a headless browser in the server image is a large dependency and
attack surface for a case that real traffic has not shown.

**Verdict: PARTIAL.** Revisit on evidence of extraction failures, not
speculatively.

### 8. Sentence-level compression: SKIP for now

Their compression eval is well designed *for their pipeline*: it measures
trimming of already-retrieved passages, so its dataset is deliberately
post-retrieval (5 pre-scored ~400 B chunks spanning several URLs). That is the
correct input for that question.

It is the wrong fit for aimee because `web_read` already returns ~1500 bytes of
match-centred windows. Compressing that further risks removing the qualifying
clause next to the answer for a saving measured in hundreds of bytes.

**Verdict: SKIP.** Revisit only if window budgets are raised.

### 9. Semantic cache: SKIP

`semcache.py` (bi-encoder shortlist + NLI cross-encoder verification) is careful
work. It is also the highest-risk thing in their repo to adopt: a false hit
serves the wrong answer confidently. Exact caching must earn its keep first, and
"similar query" is not "same query" under different scope, freshness, or
authorization.

**Verdict: SKIP** until item 2 has shipped and shown value.

### 10. Volatility-classified TTLs: SKIP

`volatility.py` picks a TTL from inferred content volatility (15 min for prices,
90 days for specs), using cue rules plus a nearest-centroid embedding classifier
shipped as a ~25 KB artifact.

Genuinely clever, and premature here: aimee has no cache at all, so there is
nothing to tune. Static per-surface TTLs first.

**Verdict: SKIP.** Reconsider after observed staleness complaints.

### 11. Model-contributed cache (`save_finding`): DO NOT ADOPT

Lets the model write answers into the cache marked UNVERIFIED.

aimee's promotion path makes this actively unsafe: `learning_judge_commit`
promotes on **corroboration count**, with no source-trust dimension. Two agents
reaching the same wrong answer, easy, from the same poisoned page or a shared
hallucination, would promote it to `committed`. There is no UNVERIFIED state to
hold it. Adopting this without a source-trust field first would be handing the
learning pipeline a way to launder model output into fact.

**Verdict: DO NOT ADOPT** without a source-trust dimension on artifacts.

### 12. Chunk-then-rank retrieval: DO NOT ADOPT

Correct for them: they run a real bi-encoder and cross-encoder, and embedding
models need bounded inputs. Chunking is load-bearing in their design.

Wrong for aimee, and this is the mistake already made once. Chunking was
imported without the embedder that justified it, leaving fixed-size boxes
feeding a substring matcher, plus a per-chunk score that was unstable under its
own segmentation. Measured cost of the boxes alone: a token split across a cut
exists in the page but in no chunk, so no ranking can retrieve it, 1.6% of
generated pages.

Deleted and replaced with match-centred windows.

**Verdict: DO NOT ADOPT.** If a semantic leg is ever wanted, wire the existing
embedder to the extracted windows. Do not reintroduce chunking to get it.

### 13. Put today's date in the system prompt: ALREADY HAVE, worth confirming

Their strongest practical finding: without a date, models refuse to search for
events they believe haven't happened, costing up to 10/27 fresh questions. aimee
injects `currentDate` already. Free, and worth not regressing.

## What the traffic says about all of this

Measured on the 84 real pages, with a 1500-byte budget:

| | |
|---|---|
| queries producing ≥1 match | 92% |
| zero-match queries | 8%: every one a whole-document request |
| windows per query | p25 1, median 3, p75 8, p90 15, max 30 |
| queries with ≤4 windows (selection irrelevant) | **58%** |
| queries with >4 windows (selection decides) | **42%** |

Two things follow. Deterministic extraction covers most real traffic, so the
elaborate ranking stack is not the missing piece. And selection genuinely
matters for a large minority, which is why `web_read` now orders by distinct
query-term coverage, worth **+56.7%** more distinct terms surfaced than
document-order truncation on that corpus (33 pages better, 2 worse, 21 unchanged).

## Recommended order

1. Egress policy split for `web_search`, blocker for everything below.
2. Fuse `web_read` extraction into `web_search`.
3. Result cache, with provenance and age.
4. Multi-engine fanout with URL dedup, reusing `kb_rrf`.
5. Engine circuit-breaking.
6. Savings counters.

Everything else is SKIP or DO NOT ADOPT on current evidence.

## Limitations

- The corpus is one operator's agent traffic, weighted to GitHub and technical
  docs (47 of 94 fetches). Not a general web sample.
- Query shapes come from one tool's prompt field, which may steer phrasing.
- No semantic baseline was run head-to-head. The claim is that chunking costs
  correctness without an embedder, not that embeddings never help.
- Their cost and token headline numbers were not reproduced.
