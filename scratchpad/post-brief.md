# Brief: Reddit post on chunking vs deterministic extraction

## Job
Draft a Reddit post (r/LocalLLaMA or r/programming style) presenting measured
findings about chunk-and-rank retrieval vs deterministic match extraction for
the "agent reads a web page" case.

## VOICE — this is the author's voice, match it
- Blunt, terse, technical. First person. Opinionated but backs everything with a number.
- Short paragraphs. No preamble, no throat-clearing, no "in today's world".
- NO AI-isms. Banned: "delve", "leverage", "robust", "seamless", "it's worth noting",
  "in conclusion", "landscape", "game-changer", "unlock", "empower", "dive into",
  "let's explore", em-dash-heavy hedging, tricolon lists, "not only... but also".
- No bullet-point soup. Prose with a few tables/numbers where they earn it.
- Mild profanity is fine and in character. Do not force it.
- Do not sound like a press release or a paper abstract. Sounds like an engineer
  posting what they found after a long day.
- No "I hope this helps", no signature, no emoji.

## HARD RULES ON CLAIMS — the author has been burned by overclaiming this week
- Do NOT claim their cost/token headline numbers are wrong. They were never tested.
- Do NOT claim ranking is useless. The data says it matters for 42% of real queries.
- Do NOT say "chunking is always wrong". Say what the data says.
- Every number in the post must come from the DATA section below. Invent nothing.
- If a claim is not in the data, cut it.
- State limitations explicitly near the end. That is part of the voice, not a hedge.

## THE ARGUMENT (this is the spine — narrow and defensible)
1. Chunking in these pipelines exists to feed embedding models. If you are not
   running an embedder, chunks buy you nothing and cost you correctness.
2. Their published compression eval does not test page-scale retrieval. It runs on
   pre-ranked output, so the retrieval problem is already solved before the
   dataset exists. That is a methodology criticism, verifiable from their repo.
3. On real agent traffic, most queries do not need ranking at all. A meaningful
   minority do. Both halves get said.

## DATA — use these, do not round differently, do not embellish

### Their eval dataset (from their own repo, evals/datasets/compression_chunks.jsonl)
- 50 rows. Each row = exactly 5 chunks, ~400 bytes each (median chunk 396B).
- All 50 rows span MULTIPLE source URLs (4-5 distinct sites per row).
- Every chunk already carries a relevance `score` — they are the pipeline's top-N.
- Reconstructed "page" size: min 1641B, median 1974B, max 1992B.
- Conclusion: this measures sentence trimming of already-retrieved passages, not
  finding relevant content in a document.

### Real-world corpus (agent tool-call history, this is the novel data)
- 304 agent session transcripts, 35,555 total tool calls.
- 205 web tool calls: 104 page fetches, 101 searches.
- 94 unique (url, prompt) pairs; 84 pages fetched successfully.
- Real page size: min 83B, median 139,290B (~139KB), max 2,097,152B (2MB cap).
  Compare to their ~2KB reconstructed pages. Two orders of magnitude.
- Query shape: median 22 words, median 145 characters. 97% are 6+ words
  (natural language, not keywords). 60% contain an identifier-shaped token
  (snake_case, CamelCase acronym, or a version number) embedded in prose.
- 42 of 94 prompts are whole-document requests ("return the README verbatim",
  "list every file", "summarize this project").

### Deterministic extractor measured on that real corpus (n=84)
Method: find every case-insensitive occurrence of query terms, widen each to a
~220-byte window snapped to whitespace, merge overlaps, emit in document order
until a 1500-byte budget is spent. No chunker, no scorer, no model.
- Queries producing >=1 match: 77/84 = 92%
- Queries producing ZERO match: 7/84 = 8%. ALL SEVEN are whole-document requests
  ("return the full README verbatim"). Zero genuine extraction queries failed.
- Matches per query: p25=3, median=43, p75=112, p90=245, max=2244.
- Windows per query after merging: p25=1, median=3, p75=8, p90=15, max=30.
- Queries where every window fits the 1500B budget: 21/84 = 25%.
- Queries with <=4 windows (so ordering is irrelevant): 49/84 = 58%.
- Queries with >4 windows (so SELECTION MATTERS): 35/84 = 42%.

### The chunking failure mode (measured separately, generated pages)
- A fixed-size cut can land inside a token. The token then exists in the page but
  in no chunk, so no ranking can retrieve it. Measured at 1.6% of generated pages
  carrying an identifier at a random offset (20,000 pages).
- Match-centred windows cannot split a token they are centred on. The failure is
  structurally impossible rather than rare.

### Honest limitations — MUST appear in the post
- The 42% figure means ranking is doing real work for a large minority. Document
  order is not relevance. This is not "ranking is pointless".
- Their cost and token claims were not tested or reproduced here.
- The corpus is one operator's agent traffic, heavily weighted to GitHub and
  technical docs (24 github.com, 23 raw.githubusercontent.com of 94). It is not a
  general web sample.
- The query shapes come from one tool's prompt field, which may steer phrasing.
- No semantic/embedding baseline was run head to head. The claim is about what
  chunking costs, not proof that embeddings never help.

## LENGTH
600-900 words. Title included. No TL;DR longer than two lines.
