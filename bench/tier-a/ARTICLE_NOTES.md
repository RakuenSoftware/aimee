# Findings, with the evidence and the caveats

Working notes for a writeup. Every claim here says how big the sample was, what
was measured against what, and what is still unverified. Where a claim was made
and later withdrawn, both are kept — the retraction is usually the interesting
part.

Hardware unless stated: RTX 5080 (16 GiB), CUDA build `b10201-9-g0005475`, CT 140
on `.253`. Corpus v5 (1001-note tier), prompt v8, thinking on, `-c 8192`,
`--no-mmproj`, greedy.

---

## 1. A prompt sentence turned the model's reasoning off

`gemma-4-E4B` emitted **zero** reasoning tokens on all 10,000 notes of a run that
recorded `thinking: true`. Cause: the prompt ended `No prose, no markdown.`, and
E4B applies that to its own thought channel.

| system prompt | notes that reasoned |
|---|---:|
| v4 unmodified | 0/20 |
| minus `No prose, no markdown.` | 20/20 |
| minus `Return ONLY a JSON object:` | 0/20 |
| rescoped: "the answer itself must be JSON only" | **0/20** |
| **v5: "Reason first if it helps; the answer that follows..."** | **20/20** |

Two things make this hard to catch. **Nothing fails** — valid JSON, clean parse,
no truncation, a plausible 0.5947 F1. And **E2B does not have the behaviour**, so
the two arms of one sweep disagreed for what looked like a model-size reason.

Deleting the sentence is not the fix: it restores reasoning and returns fenced
` ```json ` on 14 of 20. The clause was doing real work; the production parser's
first-`{`/last-`}` scan merely hid it.

Reproduced on two independent builds with **different chat templates** — Unsloth
UD-Q4_K_XL (sha `74a88f94…`, 18807 B) and stock ggml-org Q8_0 (sha `603a42db…`,
18566 B). So it is the model, not a vendor's quantisation. That mattered: "it's
the UD quant" was the obvious objection and it was tested rather than argued.

## 2. The number that justified the fix was n=70 and does not reproduce

"Thinking is worth +0.084 F1 to E4B" appeared in `kb_curator_provider.c`,
`provider_client.c`, and the commit messages. Provenance: 53 true positives on
~70 notes, no interval.

Paired over 955 notes of the 10k corpus, same model, quant, card and corpus:

| | strict F1 | precision | recall |
|---|---:|---:|---:|
| v4, thinking suppressed | 0.5990 | 0.6607 | 0.5478 |
| v5, thinking restored | 0.6093 | 0.6175 | 0.6014 |

**+0.0103, 95% CI [−0.0201, +0.0404] — indistinguishable**, 5000 paired
replicates.

Stopping there would invert the same error. The error audit says 68 of v5's 93
extra false positives are reconcilable by `rel_type_canonicalize()` and the entity
graph — machinery production already runs — and only ~24 are genuinely spurious.
Scored on entity pairs, ignoring predicate naming:

| | relation-agnostic F1 | precision | recall |
|---|---:|---:|---:|
| v4 | 0.7783 | 0.8585 | 0.7118 |
| v5 | **0.8390** | 0.8503 | **0.8280** |

Recall **+0.116 at flat precision**, fabrication 0.0 on both. Thinking finds
materially more real facts and names them more variably; strict F1 charges that
variance twice. It costs abstention (0.907 → 0.870). No interval on the
relation-agnostic delta — the bootstrap tool scores strict only.

## 3. The benchmark was fast because it was broken

The v4 10k arm finished in ~34 minutes and that was read as a hardware fact.

| | v4 as banked | thinking restored |
|---|---:|---:|
| median completion tokens | **27** | ~390 |
| median latency | **214 ms** | ~1790 ms |
| notes that reasoned | **0 / 10000** | 20/20 |
| throughput | 280/min | 27/min |

**The token count is not the tell**, and reading it as one is a mistake I made
first: `{"facts":[]}` is 5 tokens and one triple is ~30, so p10 5 / median 27 /
p90 49 is a healthy extractor. `parse_ok` was 10000/10000. The answer channel was
never unhealthy — only the reasoning channel was, and the answer channel is the
one every tool looks at.

The real signal was in every row and nothing read it:

```json
{"thinking": true, "reasoning_chars": 0, "parse_ok": true, "truncated": false}
```

Self-contradictory, ten thousand times. `reasoning_chars` was added during an
earlier triage and never consumed by `score.py` or `summarize.py`. **Recording a
signal is not checking it.** The scorer now refuses a run whose rows claim
thinking with no reasoning anywhere — the fourth instance of a defect whose two
predecessors are documented in the same file as *"It recorded the field. Nothing
read it."*

## 4. The corpus phrased a hostname fact as "runs on"

28 of 51 `has_hostname` gold triples came from notes worded "X runs on Y".

| note phrasing | n | model says has_hostname | model says runs_on |
|---|---:|---:|---:|
| "X has hostname Y" | 23 | **23/23** | 0 |
| "X runs on Y" | 28 | **0/28** | 23 |

Identical in both runs. The model is perfect on one phrasing and scores zero on
the other because it reads "runs on" as deployment, which is what it means. One
template manufacturing 28 false negatives and 23 false positives per run, and
penalising exactly the models that read the sentence correctly.

They are different facts at different levels of one chain:

```
service --runs_on--> host --has_hostname--> "wol-realm-dev-9"
```

## 5. The kind gate cannot fire on the LLM path

`mf_commit_facts` sets `obj_kind = NODE_OTHER` (the extractor supplies no kinds),
then rewrites it to the relation's declared tail when OTHER is not allowed —
which is **14 of the 17 original seed relations**. `FACT_GATE_REJECT_KIND` is
unreachable for objects: the gate does not refuse a mistyped object, it relabels
it.

The behaviour stays, because without it a seed relation drops every fact it
extracts. What was wrong was the comment claiming validation. **Inference was
measured before being rejected**: across 1333 seed-relation triples the only tail
a text rule can confidently judge is `device_has_ip`, and all 96 of those objects
are valid IPs — a text-based check would have caught **zero**. The mistypings
that do occur (`member_of enterprise`, `has_role <contract>`) are wrong in a way
no kind rule can see.

The stored kinds are inert: nothing reads `entity_edges.subject_kind/.object_kind`
back, for gating or recall.

## 6. The ontology did not cover the domain

**19% of the GOLD's own triples** (167/880, 12 predicates) used relations the seed
ontology did not define — `owns_account` 39, `subscription_tier` 39,
`customer_of` 26, `purchased` 17. So the benchmark made the model invent a word
and graded it on guessing the same one. The gold was not even self-consistent:
both `owns` and `owns_account` appear.

The model mirrored it: **22–24% of extracted facts** used a non-seed predicate, 89
distinct ones over two 1k runs, 54 seen exactly once.

These facts were never stranded — a NOVEL verdict still writes the edge and
recall filters on `superseded_at`/`suppressed`, not on class. (I claimed
otherwise first and it was wrong.) The cost is **fragmentation**:

| family | facts | split across |
|---|---:|---|
| hosting/deployment | 112 | runs_on 45, has_hostname 46, operates 16, hosts 5 |
| ownership | 89 | owns 59, acquired 30 |
| membership | 396 | works_for 205, member_of 167, contributes_to 24 |

And §7.2 auto-promotion (threshold 3) would have made it permanent: 23 of the 89
recur often enough to become active relations, taking the ontology to ~40
mostly-synonymous entries.

Seeded seven (`customer_of`, `subscription_tier`, `owns_account`, `purchased`,
`founded`, `mentors`, `runs_on`) plus 15 aliases. **Deliberately not folded**:
`owns` (too generic), `operates`/`runs` (running a *business*, not running *on* a
host), `contributes_to` (not membership).

**Early result: novel-predicate rate 23.5% → 10.0%** at n=223 under 24 relations.
Provisional; the full arm was interrupted.

## 7. Production filed one entity under three names

`entity_name_normalize()` lower-cased and collapsed whitespace and **nothing
else**. So `Sunshine`, `Sunshine team` and `sunshine_team` were three
`canonical_id`s, and every fact under one was invisible to a query about another.

The benchmark's scorer had folded separators, articles, honorifics and edge
punctuation for months. The corpus was cloned **from production data**, so those
folds describe real mentions — production was simply the one not doing them.
Which also means **tier-A extraction measured cleaner than the graph it
produced**.

The descriptor list is deliberately narrow and the omissions are the point:
`van`, `gateway`, `router`, `server`, `box`, `project` are excluded because
product names carry those words inside them (`Girder Gateway van`, `Ingot
Router`). A missed fold leaves two nodes and an alias can join them later; a
wrong fold welds two real entities together with nothing left to undo it from.

## 8. The migration for that fix stranded facts, and only real Postgres showed it

`db2_entity_renormalize_aliases()` merged the registry and stopped. But
`entity_edges` stores endpoints as **text** and recall matches them literally —
`db2_fact_recall_block` and `db2_fact_current_count` both do `WHERE source = ?`
with no canonicalisation. A fact written under the losing display name stayed
filed under a name nothing resolved to any more: **present in the table,
invisible to every query.**

Silent memory loss, shipped by a migration whose purpose was to stop losing
memory. **The shim test passed throughout** because it had a legacy alias row and
no legacy *edge*. The Postgres integration test was written to seed exactly that
and failed on the first run.

Second time in one session that a real-backend test caught what the sqlite shim
could not.

## 9. Retractions never reached the storage layer

`db2_fact_retract()` has existed and been tested since P3 — bitemporal supersede,
immutable-edge refusal, a §4/§5 authority guard so a model cannot delete a
user-stated fact. **Nothing on the LLM path called it.** `fact_ingest.c` calls it
only from the pattern extractor, and only for first-person `user` attributes. So
"I no longer work at X" was handled by regex, while "Kestrel Freight isn't a
customer any more" — every third-party fact, which is most of them — was dropped
by a prompt telling the model a retraction had nothing durable to record.

`member_of` is multi-valued, so nothing supersedes it: the edge stayed ACTIVE
however many notes said it was over.

The models were already finding the fact. On the negation slice they emitted
either the correct triple with the polarity dropped (`Kestrel Freight member_of
customer`) or an invented negative predicate (`removed_from`, `deleted_from`).
Both were discarded.

**Polarity on the original fact** maps 1:1 onto the existing API, and keeping the
*object* is the point — `target` scopes the retraction to one edge, where NULL
retracts every value of `(source, relation)`.

Measured on two models, corpus v5, 1001 notes:

| | retractions flagged | usable by `db2_fact_retract` | polarity errors / 869 ordinary notes |
|---|---:|---:|---:|
| E4B | 115/132 | 92 | **0** |
| E2B | 85/132 | **85 (100% of flagged)** | **1** |

Different failure profiles, both safe. **1 error in 1738 non-retraction notes.**
Moves emit both halves correctly paired — retract the old location, assert the
new — 85% of the time.

## 10. The corpus cannot be reproduced

`generate_gold.py` is seeded and deterministic and the README says so. Its
`--inventory`/`--synth` inputs were **never tracked in git**. Four surviving
inventory files were tried against the recorded seed: **0 of 1001 notes match**.

So v5 was *derived* from v4 (368 relabels, 0 note-text diffs, 0 id diffs) rather
than regenerated. Ids and note text are stable v4→v5 — unlike every earlier
version bump — so a v4 prediction file can be scored against v5 gold.

## 11. Determinism: fresh servers reproduce, warm ones do not

The most transferable finding here, and it came from a challenge to a claim I had
made too quickly.

I measured 32 parallel slots at **4.54× wall-clock** (43.8 min → 9.6 min) with
**197 of 1001 notes extracting different facts**, and attributed the difference to
concurrency. That attribution had no control.

The control:

| comparison | identical raw |
|---|---:|
| banked arm vs fresh pass 1 | **20/20** |
| banked arm vs fresh pass 3 | **20/20** |
| pass 1 vs pass 3 (both fresh) | **20/20** |
| banked arm vs pass 2 (warm) | 14/20 |
| pass 3 vs pass 4 (warm) | 14/20 |

**Greedy decoding on this stack is bit-reproducible across independent server
restarts.** It is *not* reproducible against a server that has already served
requests: exactly 6 of 20 drift, and reproducibly so.

Mechanism: llama.cpp reuses a cached prompt prefix per slot. Every request here
shares the same ~600-token system prompt, so whether a request recomputes its
prefix or reuses cached KV depends on what ran before it, and those paths do not
produce bit-identical logits.

Consequences:
- Every benchmark arm is valid — the drivers restart the server once per arm.
- A "just re-run those few notes" spot-check against a warm server is **not** a
  valid check, and would manufacture phantom disagreements.
- The 32-slot comparison changed concurrency *and* the cache-reuse pattern
  (1 slot reused vs 32 slots with varying state), so 197 is an upper bound on the
  concurrency effect, not a measurement of it.

## 12. MTP: available, enabled by an undocumented path, 1.83x, and not free

Three wrong conclusions were reached before the right one. All three are kept
because the sequence is the point.

**Wrong #1: "no gemma-4 MTP in this build."** Based on grepping `libllama.so` for
`llama_model_<arch>9graph_mtp` symbols, which listed cohere2moe, glm_dsa, hy_v3,
mimo2, qwen35, qwen35moe, step35 -- no gemma4. But gemma-4 MTP is not implemented
as a nested MTP graph. `src/models/gemma4-assistant.cpp` is a full model that
reads `LLM_KV_NEXTN_PREDICT_LAYERS`, and `gemma4.cpp` notes that "MTP draft
contexts can read it via `llama_get_embeddings_nextn_ith()`". The grep was too
narrow.

**Wrong #2: "no MTP commit in the history."** `git log --grep=mtp` returned one
unrelated SYCL commit -- from a SHALLOW clone. Truncated history, meaningless
answer.

**Why `-md` could never work.** `--mtp` is registered for
`LLAMA_EXAMPLE_DOWNLOAD` only, so `llama-server` has no such flag and it never
appears in `--help`. The speculative type is inferred from the DOWNLOAD PLAN, not
from the model file (`common/arg.cpp:549`). And an explicit draft file actively
suppresses that inference:

    "an explicit draft file selection (e.g. -md with -hfd) disables the sidecar
     resolution of the draft repo"   ->  plan_spec.mtp = {}

So `-m model.gguf -md mtp-head.gguf` -- the obvious invocation -- is the one
combination that cannot reach MTP. It loads the head as a generic draft model,
which is exactly what the log said: "failed to measure DRAFT MODEL memory".

**What works.** Sidecar discovery runs only when a draft HF *repo* is set
(`arg.cpp:398`), because `plan_spec` is built from
`params.speculative.draft.mparams`:

    llama-server -hf unsloth/gemma-4-E4B-it-GGUF:UD-Q4_K_XL \
                 -hfd unsloth/gemma-4-E4B-it-GGUF          # no -md

`/slots` then reports `speculative: true`. The resolved head is
`mtp-gemma-4-E4B-it.gguf`, arch `gemma4-assistant`, paired with the UD quant the
ladder actually measures (5,126,306,944 bytes -- identical to the local copy), so
adopting MTP does not silently change the quant.

**Wrong #3: "speculative decoding is output-identical under greedy."** It should
be -- verification accepts a drafted token only if it equals the target's argmax.
Measured on 100 notes, fresh servers, concurrency 1:

| | identical to banked sequential arm | wall |
|---|---:|---:|
| plain (no MTP) | **100/100** | 263 s |
| MTP | **74/100** | **144 s (1.83x)** |

The theory misses that verification feeds SEVERAL tokens per forward pass instead
of one. The target's batch shape changes, the reduction order changes, near-ties
flip. **Speculation and parallel slots perturb outputs through the identical root
cause** -- batch shape -- and neither is a numerical-precision curiosity that can
be waved away at 26% of notes.

1.83x is also, exactly, the figure in the pre-existing `overnight_10k.sh` comment
about "client-side batching". Possibly coincidence; possibly that measurement was
also a batch-shape change and was recorded under the wrong name.

**But it is self-consistent, on both models.** Two MTP runs, fresh server each:

| model | run 1 vs run 2 |
|---|---:|
| E4B UD-Q4_K_XL | **100/100** |
| E2B UD-Q4_K_XL | **100/100** |

E2B was checked rather than assumed -- `/props` confirmed
`gemma-4-E2B-it-UD-Q4_K_XL.gguf` was actually loaded (median latency 1345 ms
against E4B's 2548 ms), because `--model` is only a label and a stale server
would have silently produced E4B twice and a meaningless pass.

So the perturbation is systematic rather than random, and a ladder run entirely
under MTP is internally comparable across both model families. The 1.83x is
usable; those arms simply cannot be compared against the sequential arms already
banked.

### The three configurations, and what each costs

| config | speed | vs sequential | self-consistent |
|---|---:|---:|---|
| sequential | 1.00x (43.8 min/arm) | identical by definition | yes, 4 confirmations |
| MTP | 1.83x (~24 min/arm) | 74/100 | **yes, 100/100** |
| 32 slots | 4.54x (~9.6 min/arm) | 804/1001 | untested |

The decision is not "fast or accurate". It is: comparisons WITHIN a
configuration are sound; comparisons ACROSS configurations are not. Whatever is
chosen has to be held fixed for every arm, and recorded in the results, or the
quant deltas being chased (~0.01 F1) are smaller than the configuration noise
(~20-26% of notes).

## 13. (superseded by 12 -- kept only as the retracted claim)

The original conclusion here was "gemma-4 MTP is not in this build". It is. See
above for how that was reached and why it was wrong.

Passing `-md mtp-gemma-4-E4B-it-Q8_0.gguf` loads the head and then silently
disables speculation — `/slots` reports `speculative: false`,
`speculative.types: "none"` after `[spec] failed to measure draft model memory:
failed to create llama_context from model`.

Architectures with an MTP graph in `libllama.so` (`b10201-9-g0005475`, 2026-07-31):

```
cohere2moe, glm_dsa, hy_v3, mimo2, qwen35, qwen35moe, step35
```

**No gemma4.** The head declares arch `gemma4-assistant`, which the build knows
how to *load*, but there is no MTP path to run it through. An MTP head is not a
standalone model — it shares the base model's embeddings — which is why creating
a context for it fails. Generic `-md` cannot substitute for architecture support.

Open: whether the upstream gemma-4 MTP merge postdates this snapshot. Resolving
it means rebuilding llama.cpp from current master.

---

## The pattern worth writing about

Nine of these are the same shape: **a signal was recorded and nothing read it.**

- `reasoning_chars: 0` written 10,000 times next to `thinking: true`
- `-ngl 99` recorded as provenance while an iGPU served the requests (defect 30)
- a device.json that could not distinguish two cards
- an F1 that was a thinking-off number in a file that said thinking-on
- entity kinds written to columns nothing reads
- a corpus README promising reproducibility from inputs nobody kept

And four are the other shape: **a check that could not fail.** The kind gate that
always coerces. The scorer that could not see suppressed reasoning. The shim test
with no legacy edge. The alias migration that merged registries and left the
edges behind.

The most expensive single lesson: **n=70 with no interval became a constant
quoted in three source files.** The second: **testing an API and testing a prompt
separately is how a missing connection between them stays invisible** —
`db2_fact_retract` was complete, tested, and unreachable for months.
