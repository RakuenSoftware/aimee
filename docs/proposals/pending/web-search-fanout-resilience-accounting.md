# Design brief: multi-engine fanout, circuit-breaking, provenance, accounting

- **State:** PENDING — decision brief; the proposed default path, resilience, provenance, and
  accounting changes remain open.

Items 3–6 of [web-retrieval-capability-map.md](../done/web-retrieval-capability-map.md).
Items 1 and 2 (search→extract fusion, page cache) merged in #1830.

This brief asks for decisions on seven points. Each states what I propose and
what I am unsure about. Where I have measurements I give them; where I am
guessing I say so.

## D0. The fusion path built in #1830 has no caller — what should the default be?

**This is the most consequential item here and it is not one of 3–6.**

`web_search_ex(query, max_results, fetch_pages, extract_query)` exists and is
tested, but `td_web_search` in `agent_tools_dispatch.c:1286` calls plain
`web_search(...)`, which hardcodes `fetch_pages = 0`. The tool schema
(`tp_web_search`) exposes only `query` and `max_results`. So the capability the
map called "the single highest-value item" is currently unreachable by any
agent. It ships dead.

Options:

- **(a) Default on.** Every `web_search` fetches the top 3 results and returns
  extracted page text after the snippet block. Best token-per-answer and the
  agent stops making a second `web_read` round trip. Costs several seconds on
  every search — measured design budget is 3s/page, 8s total, serial.
- **(b) Default off, model-controlled parameter.** Add `fetch_pages` to the tool
  schema. Zero latency regression; relies on the model choosing it, and a
  capability the model must opt into is usually a capability that goes unused.
- **(c) Default on, with an escape.** Fusion on by default, `fetch_pages: false`
  available to opt out for latency-sensitive lookups.

**I propose (c).** The guidance I am working to is that defaults should be best
for user experience, and a search that returns the answer beats one that returns
links to the answer. But this is a latency-for-quality trade on a very common
path and I do not think it is mine to make unilaterally.

**Unknown I cannot resolve from data:** real traffic has 101 searches and 104
page reads, which is consistent with "most searches are followed by a read" but
does not prove the read targets a top-3 result. If it usually does not, (a)/(c)
pay latency for pages nobody wanted.

## D1. Fanout has nothing to fan out to on a default install

`config.search_backend` is a **single string**. Default is `duckduckgo`;
`searxng` needs a URL and `tavily` needs an API key. So on a default install
there is exactly one usable engine and multi-engine fanout is a no-op.

Item 3's value is therefore conditional on configuration that most installs do
not have. Options:

- **(a) Add `search.backends` (a list), fan out to all configured.** Honest, and
  does nothing until someone configures a second engine.
- **(b) Fan out to whatever is credentialed**, deriving the set rather than
  adding config. Less config surface, but silently changes behaviour when
  someone adds a Tavily key for another reason.
- **(c) Do not implement fanout; implement dedup + normalisation only**, which
  is the deterministic half and pays off within a single engine's results too
  (DDG does return duplicate URLs across result pages).

**I propose (a) plus the dedup half of (c) unconditionally.** I want to flag
clearly that under (a) item 3 delivers no measurable benefit to a default
install, which makes it lower value than its "ADOPT / the one place rank fusion
belongs" verdict implies.

## D2. RRF's id field is 256 bytes; URLs are not

`kb_rrf_item_t.id` is `char[256]`, compared with `strcmp` and copied with
`snprintf`. Normalised URLs can exceed 255 bytes, and truncation would **merge
distinct URLs sharing a long prefix** — a correctness bug, silently fusing two
different pages into one candidate.

Options: truncate (rejected — the failure is silent and wrong); hash to 64-bit
hex (collisions merge distinct pages, same failure at lower probability); or
**assign each distinct normalised URL a first-seen index and use a zero-padded
decimal as the RRF id** (`"%03d"`).

**I propose the index.** It is exact, needs no change to `kb_rrf.c`, and
zero-padding makes `strcmp` order equal numeric order, so RRF's documented
`id asc` final tie-break resolves toward the URL seen earliest — i.e. the
higher-ranked hit from the first engine — instead of resolving arbitrarily.

## D3. How aggressive should URL normalisation be?

Dedup quality is entirely this decision. Under-normalise and dedup does nothing;
over-normalise and distinct pages merge.

**Proposed (conservative):** lowercase scheme and host; strip default port; drop
fragment; strip a trailing slash on an empty path; treat `http` and `https` as
**distinct**.

**Deliberately NOT proposed:** stripping `www.`; stripping tracking parameters
(`utm_*`, `fbclid`); sorting query parameters; unifying `http`/`https`.

Each of those is *usually* safe and *sometimes* wrong (a site serving different
content on `www`; a query parameter that looks like tracking but selects
content). I lean conservative because a false merge loses a result silently,
while a missed merge only costs a duplicate line. **Is that the right side to
err on here?** `db1_web_page_canonical_url` already implements exactly the
conservative rules, so reusing it keeps cache identity and dedup identity the
same — which I think is worth more than marginal dedup.

## D4. Circuit breaker: where does the state live?

Item 4 wants a consecutive-failure breaker with cooldown and a half-open probe,
counting **empty results as failure** (a scraper returning 200-with-nothing is
telling you it has decided you are a bot).

The design question is state location:

- **(a) Process-local static.** Simple, no schema. Lost on restart, and not
  shared across processes.
- **(b) DB1 table.** Survives restart, shared. Heavier, and a breaker that
  persists a stale "engine is dead" verdict across a restart may suppress a
  working engine.

**I propose (a).** The breaker's whole purpose is to avoid hammering a dead
engine inside a working session; forgetting on restart is acceptable and
arguably correct. I want this checked — the counter-argument is that a server
restarting frequently never accumulates enough state for the breaker to fire.

Thresholds (all guesses, and I will label them as such in the header): 3
consecutive failures to open, 60s cooldown, one half-open probe.

## D5. Provenance — what is actually left

Cache age is already emitted (`web_read.c:570`, "served from cache, fetched N
seconds ago"). The source URL is **not** inside the extract block; it appears
only in the fusion caller's `[N] url` line, and `web_extract.h` documents `url`
as "used only for a log line".

**Proposed:** emit the source URL in the extract block header so a block is
self-describing wherever it is quoted. Small. Anything more?

## D6. Savings accounting — what is a fact and what is a story

The map is explicit that this must report **observed** quantities, not a
counterfactual "what hosted search would have cost", which is unfalsifiable.

**Proposed facts:** cache hits vs misses, bytes served from cache, bytes fetched
over the network, extracted bytes returned vs page bytes stripped.

**Explicitly refused:** any "you saved $X" figure.

Open: **where does this surface?** A log line (invisible), the tool output
(costs the agent tokens to report token savings — self-defeating), or DB1
counters queryable by `aimee insights` (consistent with existing token
accounting, more work). **I propose DB1 counters + `aimee insights`**, but this
is the item I am least confident is worth building at all. If the panel thinks
item 6 is not worth its complexity, I would rather hear that than build it.

## What I am not proposing

Items 7–13 keep their map verdicts. In particular no Playwright (item 7), no
semantic cache (item 9), and no model-contributed cache (item 11) — the last
being actively unsafe until artifacts carry a source-trust dimension, because
`learning_judge_commit` promotes on corroboration count alone.
