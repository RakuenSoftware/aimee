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

### 32 slots + MTP: does not stack, and is not repeatable

Tested because the expectation was that they would compound. Both parts of that
turned out false, measured on 100 notes, fresh server each run:

| config | speedup |
|---|---:|
| MTP alone | 1.83x |
| 32 slots alone | 4.54x |
| **32 slots + MTP** | **4.34x** |

No stacking -- the combination is marginally SLOWER than slots alone. They spend
the same resource: at batch=1 the GPU is bandwidth-bound with compute idle, and
both speculation and batching exist to fill that idle compute. Once 32 sequences
are in flight there is nothing left for drafting to claim, so verification is
pure added work.

More importantly it fails the only test that matters for a benchmark:

| comparison | identical |
|---|---:|
| run 1 vs run 2, raw completions | 63/100 |
| run 1 vs run 2, extracted facts | **75/100** |

**25 notes extract different facts between two runs of the SAME configuration.**
Wall time varied too (71 s vs 61 s), which is the mechanism: with 32 requests in
flight, batch composition depends on arrival and scheduling timing, and that is
not reproducible. MTP alone is 100/100 precisely because its batch shapes are
fixed by the draft length rather than by when requests happen to arrive.

This reframes the whole parallelism question. The requirement was never identity
with a sequential run -- it was repeatability. MTP perturbs outputs relative to
sequential and is still usable, because it perturbs them the SAME WAY every time.
Concurrency perturbs them differently every time, which no amount of speed
redeems.

### The configurations, and what each costs

MTP's speedup is model-dependent, which follows from what it exploits:

| model | sequential | with MTP | ratio |
|---|---:|---:|---:|
| E4B UD-Q4_K_XL | 22.9 notes/min | 41.9 | **1.83x** |
| E2B UD-Q4_K_XL | 27.0 notes/min | 43.0 | **1.59x** |

Speculation reclaims idle compute, and a smaller model is less bandwidth-bound at
batch=1, so there is less idle compute to reclaim. Quoting a single speedup
figure for "MTP" would be wrong; it is a property of the model, not the feature.

| config | speed | vs sequential | REPEATABLE |
|---|---:|---:|---|
| sequential | 1.00x (43.8 min/arm) | identical by definition | **yes** (4 confirmations) |
| **MTP** | **1.83x (~24 min/arm)** | 74/100 | **yes** (100/100, E4B and E2B) |
| 32 slots | 4.54x (~9.6 min/arm) | 804/1001 | **no** |
| 32 slots + MTP | 4.34x | 64/100 | **no** (75/100 against itself) |

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

## 15. Quant: the evidence is the replication, not any single interval

Corpus v5, 1001 notes, prompt v8, all arms under MTP at concurrency 1, same card,
one variable:

| | strict F1 | rel-agnostic | abstention | spurious |
|---|---:|---:|---:|---:|
| E2B Q4 | 0.6114 | **0.7728** | **0.689** | **101** |
| E2B Q6 | **0.6179** | 0.7676 | 0.661 | 112 |
| E4B Q4 | 0.6189 | 0.7421 | 0.556 | 146 |
| E4B Q6 | **0.6339** | **0.7556** | **0.578** | **139** |

Paired bootstrap, 5000 replicates:

| pair | delta | 95% CI |
|---|---:|---|
| E2B Q6 − Q4 | +0.0065 | [−0.0145, +0.0272] |
| E4B Q6 − Q4 | +0.0150 | [−0.0040, +0.0333] |

Both intervals cross zero, and reading either run in isolation would call them
indistinguishable. That reading is wrong, and it is worth being precise about
why, because it is a general trap.

**This is the 8th data point and the 5th corpus showing Q4 -> Q6 improving E2B,
always in the same direction.** Eight independent replications agreeing on sign
is roughly p = 0.008 by a sign test alone -- stronger evidence than any single
1001-note interval, and invisible to an analysis that only ever looks at one run.
A per-run CI answers "could this run have come out the other way"; it does not
answer "does this effect exist", which is what replication answers.

The corollary matters for how this benchmark is used: chasing significance
within one corpus is the wrong instrument for effects of this size. Running the
same comparison on a new corpus and checking the SIGN is cheaper and more
informative than growing n.

### The decision that follows

- **E2B: Q4.** The gain is real and consistent but small, and it costs ~1.4 GiB
  against Q6 (2.97 vs 4.39 on disk). On the E2B side that trade is not worth it.
- **E4B: Q6.** Roughly 2.3x the delta, and unlike E2B both metrics agree --
  strict AND relation-agnostic favour Q6, with better abstention and fewer
  spurious triples. E2B's two views point in opposite directions, which is what
  a genuinely marginal effect looks like.

Q4 E2B + Q6 E4B is the pairing: the memory saved on the small model is spent
where the same quant step buys more than twice as much.

---

## 16. The measurement that never ran

The sharded runner auto-sizes its process count by starting one server and
reading resident VRAM. On the 5080 it does. On the XTX it never has: the host
drives the card through Vulkan and has no ROCm tooling installed, so

    ssh admin@192.168.1.254 rocm-smi --showmeminfo vram
    bash: rocm-smi: command not found

Every XTX arm logged the result faithfully and nobody read it:

    [21:04:01Z]   one instance uses  MiB of 24560 MiB
    [21:04:01Z]   -> running 3 processes

`uses  MiB` -- an empty string where a number belongs, in every arm, all night.
The `2>/dev/null` on the probe swallowed the error, the empty value failed the
`-gt 0` test, and the script took its silent fallback. The shard counts in the
sizing script's output were reported as measured and were nothing of the kind.

Worth writing about because the failure is not the missing tool. It is that the
script had a fallback for "could not measure" and used it without ever making
the operator confront it, so a guess wore a measurement's clothes for an entire
night of runs. The number happened to be survivable. The next one might not be.

Fix applied: probe guarded by `command -v`, and the fallback now says the shard
count is a guess in the words "This arm's shard count is a GUESS."

## 17. Two sessions, one GPU, mutual assured destruction

Every arm in the sharded harness opened with

    pct exec 140 -- pkill -f llama-server

CT 140 turned out to be shared. Another session was serving
`gemma-4-E4B-it-UD-Q6_K_XL` on port 8099 throughout, and each of our arms killed
it on startup. Symmetrically, our E2B Q4 arm died mid-run and produced a
10,000-row prediction file in which 9,725 rows were transport errors -- the row
count said complete, and only the errored-row gate caught it.

The bug is `-f llama-server` matching on process name in a namespace we do not
own. Killing by port touches only what we started. Both scripts now do.

This is the concrete version of an abstract benchmarking rule: an exclusive
resource you did not verify is exclusive is a shared resource. Nothing in the
harness asserted the GPU was ours, and nothing detected that it wasn't -- the
evidence arrived as an unexplained collapse in an unrelated arm.

## 18. What the 10k ladder actually cost

Banked clean, corpus v5, 10,000 notes, 3 isolated MTP servers each:

| arm | strict F1 | wall |
|---|---:|---:|
| E4B UD-Q4_K_XL | 0.6324 | 172m |
| E4B UD-Q6_K_XL | 0.6450 | 164m |
| E4B UD-Q8_K_XL | 0.6321 | — |

The Q6 > Q4 direction on E4B holds at 10k, now the 9th replication of that sign.
E4B Q8 completed later on the XTX: 10,000 rows, zero errored rows, strict F1
0.6321 -- **0.0129 below Q6 and 0.0003 below Q4**. The ladder is not monotonic in
bit width on this task. One run on one corpus; by finding 15's own argument that
is an observation, not a result, until a second corpus reproduces the sign.

Remaining arms (E2B Q4/Q6/Q8) re-run on the XTX alone after the container was
surrendered. E2B Q4 was ~2,400/10,000 at 2026-08-03T13:40Z.

## 19. The noise floor is zero, and the number that motivated the question was contamination

`harness/noise_floor.sh` was written to settle a threat to finding 15: the same
E2B UD-Q4_K_XL arm appeared to have scored 0.6114 on one server and **0.6213** on
three. A 0.0099 swing on an unchanged measurement would have been larger than
both quant effects the campaign was chasing, and would have meant the
"8 replications, same sign" argument was reading noise.

The 0.6213 does not exist. It came from the quant ledger *before* the shared
`.shards` contamination was found -- quarantined in `1125ea2aa`, rebuilt in
`cc7f09fee`. The rebuilt ledger arm scores 0.6138. The premise in the script's
own header is stale and should be read as a record of what we believed, not of
what was measured.

What the experiment did establish, on three independent runs of that arm:

| pair | raw completions identical | strict F1 |
|---|---:|---|
| ledger_3srv vs shard3_run1 | 1001/1001 | 0.6138 = 0.6138 |
| ledger_3srv vs shard3_run2 | 1001/1001 | 0.6138 = 0.6138 |
| shard3_run1 vs shard3_run2 | 1001/1001 | 0.6138 = 0.6138 |
| any of the three vs single_run1 (1 proc, 1 slot) | 652/1001 | 0.6138 vs 0.6033 |
| any of the three vs v8base (1 proc, **4 slots**) | 645/1001 | see caveat |

So:

- **Run-to-run noise within the three-process configuration is zero**, not small.
  Three runs, days apart, across server restarts, byte-identical on every note.
  Finding 15's sign test is not counting noise.
- **The one-vs-three-process difference is 0.0105 F1** and moves 349 of 1001
  notes -- larger than either quant effect the campaign chases. Arms compared to
  each other must share a process count.
- Hypothesis A in the script header ("same config twice") is answered.
  `single_run2` is still needed for hypothesis B, whether one process reproduces
  *itself*. Until it lands, 0.0105 is a distance between configurations, not a
  demonstrated constant.

**CAVEAT, and it invalidated a first draft of this finding.** The obvious
single-server reference is `results/v8-baseline/E2B.UD-Q4_K_XL.mtp`, and it is
not one. Its `E2B.UD-Q4_K_XL.mtp.device.txt` records `total_slots : 4`. It is a
four-slot shared-batch run, the configuration finding 12 shows is not
reproducible by construction, so its 0.6114 is not comparable to either the
one-process or the three-process arms and the tidy "0.0024 gap" computed against
it was meaningless. This is finding 3's defect class again -- a plausible number
from an apparatus nobody checked -- and it was caught only because `single_run1`
landed at 0.6033 and forced the question. **Check `total_slots` in the device
record before using any banked arm as a reference.**

Method: `results/noise-floor/`, compared against
`results/quant-ledger/v5small.E2B.UD-Q4_K_XL.pred.jsonl` and
`results/v8-baseline/E2B.UD-Q4_K_XL.mtp.pred.jsonl`, keyed by note id on the
`raw` field. All four files are 1001 rows.

## 20. The parallelism limit was never VRAM. It was an 8 GiB per-process default nobody set

Two servers died mid-arm on 2026-08-03, minutes apart, on different ports. Their
logs end mid-task with no error, no stack, no OOM message. The arm kept running
and kept recording transport errors, six on one shard, one on another.

The reflex diagnosis was VRAM, because that is what you size a GPU benchmark by,
and the harness already carries a comment saying the XTX has no VRAM probe. It
was wrong. The evidence, per server:

| | |
|---|---|
| `RssAnon` | 7,422 MB |
| `RssFile` | 158 MB |
| `VmSwap` | 1,260 MB |

`-ngl 99` puts the weights on the card, so the model contributes almost nothing
to host RSS -- 158 MB of file pages for a 3.0 GiB GGUF. The 7.4 GB is anonymous
and therefore not reclaimable; it can only go to swap, and swap was full.

The server log says exactly what it is, over and over:

    srv alloc: - making room for prompt cache entry, removing oldest entry
               (size = 27.482 MiB)

`llama-server --cache-ram` sets "the maximum cache size in MiB (default: 8192)".
A host-side prompt cache of KV snapshots, ~27 MiB each, **8 GiB per process**,
never set by anything in this harness.

    3 servers x 8 GiB = 24 GiB   on a 31.4 GiB host  -> survives, thrashing
    4 servers x 8 GiB = 32 GiB   on a 31.4 GiB host  -> exceeds RAM

That is the whole explanation for both deaths, for the earlier decision to drop
from 4 processes to 3, and for the throughput collapse from 67 to 35 notes/min
that we first explained away as ordinary contention. Setting `--cache-ram 512`
took server memory from 21.8 GB to 6.75 GB, available RAM from 1.0 GB to 22.4 GB,
and throughput back to 69 notes/min.

It also resolves a contradiction we could not explain: E4B UD-Q8_K_XL, an 8.11
GiB model, completed at 3 processes while E2B UD-Q4_K_XL at 2.97 GiB was
thrashing. Host memory is dominated by the cache cap, which is per-process and
independent of model size, so the two arms cost the same host RAM.

### Why this is finding 3's defect class, not a footnote

Nothing failed. The default is documented in `--help`. The log narrated the
eviction thousands of times. And the result was a silently truncated arm and a
halved throughput figure that we rationalised twice before measuring.

The rule it earns: **a default you did not set is a configuration you did not
choose.** The harness pinned NPROC, quant, prompt, corpus and MTP, and then let
an 8 GiB per-process allocation ride on a library default.

### Sizing, after measurement

512 MiB was chosen as "clearly less than 8192" rather than derived, and the
derivation matters. One cached entry is one request -- ~600-token system prompt,
~50-token note, ~350 tokens of reasoning -- and weighs 25-32 MiB. So this model
costs ~26 KiB of KV per token and a full 8192 context is ~213 MiB. The 8192 MiB
default is ~38 full contexts of cache on a server with ONE slot.

At 512 MiB (~19 entries) the memory problem was solved but the server still
logged **89 evictions in ten minutes**, with prompt eval alternating 28 tokens on
a prefix hit and ~515 on a miss. Raised to 1024 (~38 entries): **zero evictions**,
server RSS 7.04 GB across three processes, 15.0 GB free, swap down to 297 MB,
throughput unchanged at 67-69 notes/min.

Unchanged throughput is the expected result, not a disappointment -- a miss costs
~150-195 ms of prompt eval against ~1.5 s of decode. The cache was never a speed
problem. It was a memory problem, and the fix is now sized from the KV arithmetic
instead of from an order of magnitude.

### It is a results-affecting variable

Not just a memory knob. Whether a prefix is restored from cache or recomputed
decides the logits -- the warm-server effect measured at 14/20 notes in finding
11. `--cache-ram` therefore belongs with NPROC in the list of things that must
be held constant across compared arms, and is now recorded in the DONE line.

Not set to 0, despite that being the most reproducible choice: the ~600-token
system prompt is shared by every note and served from this cache (prompt eval
logs 33 tokens, not 600). Disabling it re-evaluates the prefix per note at ~170
tok/s, about 3.5 s each, hours per arm. 512 MiB holds ~19 entries: the hot
prefix stays, the hoard goes.

### OUTSTANDING

The three banked E4B 10k arms ran at the 8192 default. The E2B ladder now runs
at 512. **Cross-family comparison at 10k is invalid until E4B is re-run at 512.**
Within-family comparisons on either side are unaffected. Roughly 9h of XTX time.

## 22. The ranking finally has a corpus underneath it

Article 1 ranked six models. Four of them had never been run on more than 1001
notes, and two had never been run on more than 70. The E2B and E4B figures it
compared them against came from the 10k ladder. So the table was comparing
numbers taken on different corpora at different sample sizes and calling the
result a ranking.

All six now have a 10,000-note arm on the same gold, at nproc=3, cache-ram 1024,
UD-Q4_K_XL:

| model | strict F1 | wall | MTP available |
|---|---:|---:|---|
| gemma-4-E4B | 0.6301 | 159m | yes |
| gemma-4-E2B | 0.6246 | 146m | yes |
| granite-4.1-3b | 0.5627 | 21m | no |
| gemma-3n-E4B | 0.5424 | 47m | no |
| Qwen3-1.7B | 0.4591 | 361m | no |
| granite-4.0-1b | 0.4215 | 16m | no |

**Do not publish this table yet.** The last column is a confound: the two gemma-4
arms ran with speculative drafts and the other four cannot, and MTP moves 26 of
100 notes (finding 12). The no-MTP ladder now running removes it. The ordering
may well survive -- the top gap is 0.055 and the bottom is 0.14 -- but "may well
survive" is what finding 19 said before process count turned out to be worth
0.0105.

### The two numbers article 1 should actually lead with

**22:1 on wall clock for 0.04 F1.** granite-4.0-1b does 10,000 notes in 16
minutes; Qwen3-1.7B takes 361 on the same card at the same settings, and lands
0.037 F1 ahead. If the article's question is "what should we use today", that
ratio is the answer to a deployment question in a way the F1 column alone is not.
No interval was computed on the 0.037.

**Abstention, not extraction, separates the granites.** granite-4.0-1b abstains
on 31.5% of factless notes, granite-4.1-3b on 75.7%. That is most of the 0.14
between them. A model that answers when it should stay quiet loses on precision
across the whole corpus, and that is a behavioural property you can see in a
sample of ten notes -- much easier to write about than a leaderboard delta.

### Three of the six emitted no reasoning at all

granite-4.0-1b, granite-4.1-3b and gemma-3n-E4B recorded `thinking: true` and
produced zero reasoning characters on all 10,000 rows. The scorer refused all
three under the defect-31 guard; they were scored with `--allow-thinking-off`.
Qwen3 and both gemma-4 arms scored clean.

Whether that is "no thought channel" or "a channel this prompt closes" is NOT
established, and the difference matters for the article: the first is a property
of the model, the second is defect 31 recurring on three more models. One
`/props` call per model settles it.

## 23. Article 3's open list is missing the question everyone asks

Finding 12 established that MTP changes 26 of 100 notes and buys 1.59x-1.83x
depending on model, and that it is self-consistent so a ladder run entirely under
it stays internally comparable. Article 3 carries all of that.

What none of it says is whether those 26 changed notes are **worse**. Identity
was measured; accuracy never was. The article tells a reader that speculative
decoding perturbs output and is usable anyway, and the obvious next question --
"perturbed toward what?" -- is not in the text or in the open-items list.

Two lanes are now measuring it at n=10000 with strict F1 on both sides:

- **XTX**: six no-MTP arms paired against the six MTP arms banked there.
- **5080**: E2B runs both sides itself, three quants x {MTP, no-MTP}.

The second exists because article 3's own header admits "one throughput
comparison is missing because the two configurations ran on different cards".
Running both sides on one card fixes that caveat and gives an independent
replication: if MTP moves the score on one card and not the other, that is a
finding about the interaction, and a single lane could not tell it apart from MTP
simply not mattering.

`harness/compare_mtp.py` reports the trade -- F1 delta against per-stream and
aggregate tok/s -- and refuses to print a gain-per-accuracy-point ratio when the
F1 delta is inside the noise threshold. A ratio whose denominator is
indistinguishable from zero is how the +0.084 claim survived for months
(defect 32).

**A null result is a real possibility and would still be the finding.** The 26
perturbed notes can cancel in aggregate. "1.6x-1.8x for no measurable accuracy
cost" is the answer a reader wants, and it would be earned rather than assumed.

## 24. What MTP costs, what it buys, and the number that was neither

Article 3 says MTP changes 26 of 100 notes and buys 1.83x on E4B, 1.59x on E2B,
measured at one process on 100 notes. Two paired sweeps now put a proper interval
around that: **1.58x to 1.91x**, eight measurements, two backends, four process
counts each, 200 notes per config, steady state.

| card | nproc | MTP | no-MTP | ratio |
|---|---:|---:|---:|---:|
| 5080 (CUDA) | 1 | 47.6 | 30.1 | 1.58x |
| 5080 | 2 | 67.4 | 36.7 | 1.84x |
| 5080 | 3 | 59.9 | 34.4 | 1.74x |
| 5080 | 4 | 61.8 | 33.6 | 1.84x |
| XTX (Vulkan) | 1 | 40.7 | 21.7 | 1.87x |
| XTX | 2 | 63.8 | 34.7 | 1.84x |
| XTX | 3 | 78.1 | 41.2 | 1.89x |
| XTX | 4 | 83.3 | 43.6 | 1.91x |

finding 12's 1.59x sits at the bottom of that band, at the process count where it
was taken. The article can now say "1.6x-1.9x depending on card and shard count"
instead of quoting one number, which is the same correction finding 12 already
made for models and never made for anything else.

**The accuracy half is still not measured.** The 10k no-MTP arms that would give
F1 on both sides were stopped to free the card for this. What MTP's 26 changed
notes do to the score remains open, and it is the question a reader will ask
first.

### The best paragraph in this whole investigation is about the instrument

The sweep's throughput metric was rows divided by wall clock. Wall clock includes
server startup. Startup is ~30 seconds per server, so it grows with the process
count -- the exact variable under test:

| card | np1 | np2 | np3 | np4 |
|---|---:|---:|---:|---:|
| 5080 | 56s | 84s | 107s | 137s |
| XTX | 61s | 67s | 83s | 99s |

On a 200-note run that is a third of the wall clock at nproc=1 and nearly half at
nproc=4. The metric did not merely add noise; it added a bias pointing the same
way as the hypothesis being tested, which is the worst kind.

It produced two confident, wrong conclusions, both of which were reported before
being caught: that aggregate throughput peaks at two processes and declines (it
plateaus), and that four processes are slower than one (they are 30-100% faster).

Worth writing because the fix is not clever -- compute throughput from
per-request latency and process count, ignore the wall clock -- and because the
wrong version looked completely reasonable in a table.

### And the number that was neither

A 5.3x MTP speedup was reported from the 10k arms, questioned as implausible, and
is now withdrawn. It came from dividing a **completed** 10,000-note MTP average
by an **early-partial** no-MTP rate. Neither figure was wrong; the division was
meaningless.

This is the third time in this project a headline number turned out to be an
arithmetic relationship between two incomparable measurements -- after the +0.084
thinking gain (defect 32) and the 4-slot v8-baseline reference. The pattern is
always the same: two numbers exist, a ratio is taken, and nobody asks whether the
denominators match.

### The open thread

At nproc=3 no-MTP, the 10k arm ran ~13 notes/min where the sweep's steady state
is 41.2. Startup cannot explain it. While that arm was live the server accounted
for 4.7s per request and the client measured 13.7s -- nine seconds unaccounted,
a gap the 200-note sweep does not show.

Either long runs behave differently from short ones -- prompt-cache pressure at
10,000 distinct notes against 200 is the obvious suspect, and `--cache-ram 1024`
holds about 38 entries -- or that run was a transient. One 2000-note run settles
it by showing whether the rate decays with corpus position. It has not been run,
and until it is, **no 10k throughput figure in this project should be compared
against a 200-note one.**

## 25. MTP is free speed: +84% throughput, no measurable accuracy cost

The measurement finding 12 never made, and the one a reader asks for first.
Finding 12 established that speculative decoding changes 26 of 100 notes and left
open whether the changed notes were WORSE. At 10,000 notes they are not.

E2B UD-Q4_K_XL, XTX, nproc=3, cache-ram 1024, prompt v8, thinking on, v5
gold_large. The only difference between the two arms is the draft:

| | MTP | no-MTP | delta |
|---|---:|---:|---:|
| strict F1 | 0.6246 | 0.6207 | **+0.0039** |
| steady notes/min | 73.77 | 40.10 | **+84.0%** |
| median completion tokens | 464 | 467 | - |
| median latency | 2439.9 ms | 4488.6 ms | - |

Both arms: 10,000 rows, zero transport errors, zero truncated, 10,000/10,000
carrying reasoning.

**+0.0039 is inside the 0.0105 noise threshold**, so no gain-per-accuracy-point
ratio is computed. "Accuracy-neutral at this resolution" is the honest phrasing,
not "identical" -- 26 of 100 notes really do change, they just do not change for
the worse in aggregate.

Five more pairs (E2B Q6/Q8, E4B Q4/Q6/Q8) will show whether this holds across
quants and both model families, or whether Q4 was the friendly case. **Do not
generalise from one pair.**

### Why this number is trustworthy and the earlier one was not

The same comparison was reported as **5.3x** earlier the same day and withdrawn.
Three instrument problems had to be fixed before the 84% could be believed, and
all three are article material in their own right:

**The metric measured startup.** `notes/min` was rows over wall clock, and wall
clock includes server load -- about 30s per server, so it grew with process
count, the variable under test. Steady state is now computed from per-request
latency and process count instead, and the difference is printed as an explicit
`startup` line rather than absorbed. (defect 35)

**Orphaned clients were stealing the ports.** Killing a sweep left its
`run_llamacpp.py` children running on the same ports the next arm used. Fifteen
of them held this very arm at 8.8 notes/min until killed, after which it ran 40+.
Every request was served normally, it just queued, so the server's own timings
looked healthy and only the client saw it. (defect 36)

**The 5.3x itself was a ratio of two incomparable measurements** -- a completed
10k MTP average divided by an early-partial, orphan-contaminated no-MTP rate.
Third instance of that exact error in this project, after the +0.084 thinking
gain and the 4-slot v8-baseline reference.

The chain is worth writing as a chain: a contaminated measurement produced an
implausible number, the implausible number motivated a hypothesis about memory
bandwidth on Vulkan, two eight-config sweeps were built to test it, and the sweeps
were themselves biased by a startup term nobody had looked at. The thing that
broke the chain was not a better experiment. It was `ps | grep -c` and a load
average of 27 that had been sitting in plain sight for six hours.

### Caveat carried on this pair

The first ~500 rows of the no-MTP arm ran while the orphans were still alive.
Median latency is robust to 5% contamination -- recent-window medians read
4300-4400ms against an all-rows median of 4489 -- so steady state holds. The
`startup (s) = 1561` figure on that side absorbs the slow period and is
meaningless. F1 is unaffected: the rows are correct, they were merely slow.
