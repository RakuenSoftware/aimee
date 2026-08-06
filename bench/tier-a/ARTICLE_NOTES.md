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

## 26. The LFM2.5 family, and what a quant ladder actually measures

LiquidAI's LFM2.5 line, all at 1001 notes on v5 gold_small, 5080, nproc=3,
cache-ram 1024, prompt v8, no MTP, LiquidAI's own standard K-quants:

| model | Q4_K_M | Q6_K | Q8_0 |
|---|---:|---:|---:|
| 2.6B | **0.5854** | 0.5795 | 0.5750 |
| VL-1.6B *(vision, text-only)* | 0.2619 | **0.2744** | 0.2725 |
| 1.2B-Instruct | **0.1911** | 0.1771 | 0.1671 |
| 230M | *0.0022* | **0.1363** | 0.1309 |

Placed against the field on the same 1001 notes at the same process count, the
2.6B lands third overall -- ahead of granite-4.1-3b and behind only the two
gemma-4s -- while every other LFM2.5 model lands below the field's floor.

**The cliff is between 1.6B and 2.6B.** 0.2744 to 0.5854 is a doubling across a
1B parameter gap, and nothing else in this benchmark shows a step that sharp.
Article 1's question is how small an extractor can be; this line puts the answer
for this family between those two sizes rather than anywhere lower.

### The quant ladder measures output behaviour, not just precision

Three sweeps, three different pathologies, and F1 alone hides all of them:

**2.6B: clean and monotonically worse.** 1001/1001 parseable at every quant.
Recall flat (0.6057/0.6091/0.6080), precision falling (0.5664/0.5526/0.5454),
abstention falling with it (0.668/0.612/0.593). More bits make it answer more
often on factless notes, and each extra answer is a false positive. Total spread
-0.0104, sitting exactly on the 0.0105 noise threshold.

**230M: the quant chose the envelope.** Q4_K_M emitted
`<|tool_call_start|>[{"name":"facts","arguments":{...}}]` on **982 of 1001 rows**
and scored 0.0022. Q6 and Q8 emitted it on **zero** rows and scored 0.1363 and
0.1309. A 62x swing in apparent capability from the quantisation alone, with two
quants acting as controls. See defect 37.

**1.2B: non-monotonic parse failure.** Rows producing no parseable JSON at all:
57 at Q4, **407** at Q6, 266 at Q8. Q6 scores on 59% of the corpus and takes
zeros elsewhere. Not monotonic in bits, so "lower quant degrades structure" does
not explain it.

The through-line is the useful one for the article: **a quant ladder run on F1
alone will report three tidy near-identical numbers and conceal that the model is
behaving completely differently at each point.** The columns that expose it are
`parse_ok` and `schema_ok`, and no driver in this project reads either. The
detector is one comparison: alert when schema_ok diverges from parse_ok.

### VL-1.6B beats the text models at its size

The vision-language model, run text-only under `--no-mmproj`, scores 0.2744
against the text-only 1.2B-Instruct's 0.1911. Worth a line, with the caveat that
it is a different class of model and its multimodal path is unused here.

It also over-extracts badly: abstention 0.180 against the 2.6B's 0.668. It
answers on 82% of factless notes, which is where its precision goes.

## 27. The context ran out and every guard said the run was clean

A model that thinks too long does not fail. It returns nothing, and this
benchmark records that as a valid empty answer.

MiniCPM5-1B at Q4_K_M, 1001 notes:

| | |
|---|---:|
| rows | 1001 |
| parsed | 661 |
| failed to parse | 340 |
| **returned empty content** | **292** |
| **flagged as truncated** | **0** |
| median completion tokens, rows that parsed | 1980 |
| median completion tokens, empty rows | **7632** |
| prompt + completion on every empty row | **8192, exactly** |

Every empty row sat precisely on the context boundary. The model reasoned for
~7600 tokens and had no room left to answer.

### Why nothing caught it

`run_llamacpp.py` records `truncated = (completion_tokens == max_tokens)`. Every
driver passes `--max-tokens 8192`; every server starts with `-c 8192`. Completion
is therefore capped at `8192 - prompt_tokens`, about 7630 with this benchmark's
~560-token prompt. **The equality can never hold.** The flag is unreachable by
construction and has never fired once in this project's history.

So a row that hit the wall recorded as: no error, not truncated, parse failure.
Three signals, and the only one that moved was the vaguest.

### It is not confined to one model

| arm | rows at the context limit | flagged truncated |
|---|---:|---:|
| MiniCPM5-1B Q4_K_M | 292 / 1001 | 0 |
| **Qwen3-1.7B 10k** | **54 / 10000** | 0 |
| LFM2.5-2.6B Q4_K_M | 1 / 1001 | 0 |
| gemma-4-E2B Q4 10k | 0 / 10000 | 0 |

The gemma arms never come close, which is exactly why this survived a year of
benchmarking: the models the harness was built around do not exhibit it. Qwen3's
figure is already committed and sits in the published field ranking.

### The shape of the mistake

Defect 16 added a truncation refusal and its commit message read "this class of
defect fails loudly or it recurs". Defect 18 recorded that it recurred within the
hour, because the check was written over a **cause** rather than an **outcome**.
This is the third instance of the same shape: the check asks "did the model hit
max_tokens" when the question is "did this row produce a usable answer".

The outcome-shaped check is one line and cannot go stale:

    prompt_tokens + completion_tokens >= context_size   # or simply: content is empty

### What it costs a reader

MiniCPM5-1B's headline number is **0.1258**. Twenty-nine percent of its corpus
produced nothing and was scored as a miss. That number is a floor, not a
measurement, and published without this paragraph it would read as "MiniCPM5 is
bad at extraction" when what happened is "MiniCPM5 reasons past the context
window and the harness did not notice".

## 28. Two newcomers, and three guards that disagreed about what a failure is

Added to the field at 1001 notes: MiniCPM5-1B (openbmb, 926k downloads) and
SmolLM3-3B (HuggingFaceTB, GGUF from ggml-org). Both first-party, both Q4_K_M and
Q8_0 only because neither publishes Q6_K.

| model | Q4_K_M | Q8_0 |
|---|---:|---:|
| SmolLM3-3B | 0.3581 | **0.3933** |
| MiniCPM5-1B | *0.1258* | *0.1652* |

SmolLM3-3B Q8_0 places **7th**, ahead of granite-4.0-1b. MiniCPM5's figures are
italicised because they are floors, not measurements -- see below.

### SmolLM3 is the only clean quant improvement in the whole campaign

+0.0352 from Q4 to Q8, which is 3.4x the noise threshold. Precision AND recall
both rise (0.336 -> 0.377, 0.383 -> 0.411), parse health is identical at 990/1001
on both, and abstention rises 0.211 -> 0.297. The model becomes more
discriminating, not merely more talkative.

Every other quant sweep this campaign either moved within noise or moved for a
reason that was not accuracy. **LFM2.5-2.6B moves the opposite way** -- 0.5854 to
0.5750 across the same bit range -- via *falling* abstention. Same knob, opposite
outcomes, two models, one night. "Quantise to Q4, it's free" does not port.

SmolLM3 is also terse and fast: 39 median completion tokens, 8 minutes for the
arm, and zero reasoning on all 1001 rows.

### MiniCPM5's numbers are floors

292 of 1001 rows at Q4 and 115 at Q8 returned **empty content** after exhausting
the 8192-token context on reasoning. Every one scored as a miss. The lower quant
is the more verbose -- median 2679 completion tokens against Q8's 1757 -- and
starves itself 2.5x as often, so the F1 gap between its quants tracks the
empty-row count rather than extraction quality. See finding 27 and defect 38.

### Three guards, three different verdicts

The interesting part of adding these models was not their scores. It was watching
the harness's three safety checks behave three different ways on the same night:

**The thinking guard fired correctly.** SmolLM3 reports `thinking: true` and
emits zero reasoning on all 1001 rows, so score.py refused it and the driver
re-scored with `--allow-thinking-off`, logging that it did. It is the fourth
model in this field to do that, after granite-4.0-1b, granite-4.1-3b and
gemma-3n-E4B.

**The truncation guard could not fire at all.** `truncated` is
`completion_tokens == max_tokens`, drivers pass `--max-tokens 8192`, servers run
`-c 8192`, so completion caps below the equality and the flag has never once been
true in this project's history. Defect 38.

**The model-identity guard fired wrongly.** ggml-org names its file
`SmolLM3-Q4_K_M.gguf` without the size suffix the repo name implies, so the check
refused a correctly-loaded model: *"loaded 'SmolLM3-Q4_K_M.gguf', expected
SmolLM3-3B / Q4_K_M"*. Both arms failed. The only evidence was one FAIL line in a
log, and an unattended sweep would have produced a results table with a 3B model
scoring 0.393 simply absent from it -- ahead of granite-4.0-1b, so its absence
would have changed the ranking.

Fixed additively with a `VERIFY_FAM` override rather than by loosening the match,
because loosening hands back the failure the guard exists to catch: a stale
server answering with another model's weights (defect 30). The same override was
then needed for the QAT arms, where Google names files `gemma-4-E2B_q4_0-it.gguf`.

**The lesson is not "guards are bad".** It is that a guard has two failure modes
and this project had only ever thought about one of them. A check that refuses
too much is as costly as a check that refuses too little -- it just fails
quietly, into a log, instead of into a headline number.

## 29. The MoE is the best-behaved model in the field

LFM2.5-8B-A1B at Q4_K_M, 1001 notes: **strict F1 0.5198**, precision 0.5707,
recall 0.4773, abstention 0.7321.

That places it between LFM2.5-2.6B (0.5854) and granite-4.1-3b (0.5432) -- except
it is not a ranking row, for a reason given below.

### It is the only model tonight with none of the pathologies

| check | 8B-A1B |
|---|---:|
| parse_ok / schema_ok | 984 / 984 |
| tool-call envelope | 0 |
| empty content | 0 |
| rows at the context limit | 0 |
| transport errors | 0 |
| fabrication rate | **0.0** |
| in-seed ontology | 0.795 |

Every other model added this campaign failed at least one of those. The 230M
switched envelope by quant; the 1.2B lost 41% of a corpus to parse failure;
MiniCPM5 starved 29% of its rows on context; SmolLM3 was refused outright by a
naming guard. This one just worked.

### Sparsity, not size, explains the speed

8B total parameters, ~1B active. Measured: **5372 MiB resident** for one
instance, auto-sized to 2 processes, **252.5 tok/s per stream**, 30 minutes for
the arm.

Its dense 2.6B sibling runs **107 tok/s per stream**. The 8B is 2.4x faster while
being three times larger, because only the active experts are touched per token.
That is the number to put in front of anyone assuming parameter count predicts
latency.

Caveat on that ratio: the two arms ran at different process counts (2 against 3),
and the scaling sweep showed per-stream throughput rises as processes fall. The
gap is far too large to be explained by that alone, but the precise multiple
should not be quoted.

### Its profile is the one you actually want

Precision leads recall, 0.571 against 0.477, with **73% abstention**. It finds
fewer facts and is right more often about the ones it reports. For a knowledge
base that is the correct posture -- a false fact costs more than a missed one,
and the whole benchmark exists because a system was writing facts nobody checked.

Contrast LFM2.5-VL-1.6B at 18% abstention, which answers on four factless notes
in five and pays for it in precision.

### Why it is not a ranking row

It cannot run 3 processes: Q4_K_M is 5.16 GB and three copies are 15.5 GB of a
16303 MiB card before KV. As a MoE all experts stay resident, so the gemma-3n
lesson (file size >> resident) does not rescue it. The sizer chose 2, and finding
19 prices process count at 0.0105 F1 -- the same magnitude as this campaign's
noise threshold. Its gap to granite-4.1-3b is 0.023, so the ordering probably
survives, but "probably survives" is not a measurement and the table should say
so rather than quietly include it.

### One methodological note worth keeping

A partial read of this arm at 473 rows gave 0.5131; the full 1001 gave 0.5198,
a difference of +0.0067. The partial was accurate. It was still correct not to
publish it -- that could not be known in advance, and this project withdrew a
5.3x MTP headline built on exactly that shortcut a day earlier. The rule is not
"partials are wrong", it is "you cannot tell which partials are wrong until
afterwards".

### Correction to finding 29: "largest" is the wrong frame twice over

Stated in conversation and wrong: that 8B-A1B was "the largest model tested".

**It is not the largest this project has run.** Larger models were measured, all
at 70 notes on `data/gold.jsonl`:

| model | size |
|---|---:|
| Qwen3.6-35B-A3B | 35B |
| gemma-4-31B-it | 31B |
| Qwen3.6-27B | 27B |
| gemma-4-26B-A4B-it | 26B |
| gemma-4-12B-it | 12B |
| **LFM2.5-8B-A1B** | **8B, n=1001** |

What is true and worth saying instead: it is the largest model this project has
run **at a usable sample size**. Everything above it was measured on 70 notes,
where the 95% interval is near +/- 0.12 -- wide enough that the entire field
below fits inside it.

**And "largest" muddles the actual finding.** A1B means ~1B ACTIVE parameters of
8B total. The result is precisely that parameter count did not predict latency:
252.5 tok/s per stream against the dense 2.6B's 107. Calling it "the largest
model" and then reporting it as fast makes the observation sound like a paradox
when it is a straightforward consequence of sparsity. The sentence to write is
about active parameters, not total ones.

Note also that two of the larger models above -- Qwen3.6-35B-A3B and
gemma-4-26B-A4B -- are themselves MoE with 3B and 4B active. The 70-note tier
never compared them on this axis, so whether the sparsity effect scales is
unmeasured here.

## 30. QAT beats the default quant on one model and does nothing on the other

Google ships quantisation-aware-trained weights for gemma-4 as legacy q4_0.
Everything in this benchmark's history runs unsloth's post-hoc UD-Q4_K_XL. Both
are ~4 bit. Paired at 1001 notes, nproc=1, no MTP, same card, same prompt --
only the quant scheme differs:

| model | QAT q4_0 | UD-Q4_K_XL | delta |
|---|---:|---:|---:|
| gemma-4-E2B | **0.6406** | 0.6017 | **+0.0389** |
| gemma-4-E4B | 0.6194 | 0.6166 | +0.0028 |

Noise threshold is 0.0105. E2B's gain is 3.7x it. E4B's is a quarter of it.

**0.6406 is the highest gemma-4-E2B figure anywhere in this project**, at any
tier -- above the 10k MTP arm's 0.6246 and its 1001-note subset's 0.6178.

### The components say it is not a threshold shift

| | precision | recall |
|---|---:|---:|
| E2B QAT vs UD | **+0.0454** | **+0.0318** |
| E4B QAT vs UD | +0.0111 | **-0.0080** |

On E2B both rise substantially, which is a genuinely better model rather than a
different operating point. On E4B precision rises slightly, recall falls
slightly, and they cancel.

### What NOT to write

The tempting sentence is "smaller models have less redundancy, so quantisation
damage costs them more and QAT recovers more". It is plausible, it fits both
points, and it is exactly the move this project already retracted once: the
"dense models are more disciplined than MoE" finding came with a mechanism about
per-token expert routing, and the log's verdict was "a story fitted to a bug".

Two models is not a size trend. The mechanism is unmeasured and stays unwritten.

### What is safe to write

On gemma-4-E2B, quantisation-aware training is worth +0.039 F1 over the dynamic
quant this benchmark has defaulted to throughout. On gemma-4-E4B the two are
indistinguishable. **Test your own model; neither result transfers.**

That lands in the same place as the quant ladders from the other direction:
LFM2.5-2.6B got worse with more bits, SmolLM3-3B got better, and here a
differently-*trained* quant at the same bit width beats both framings. The
portable claim across all of it is that **which quant matters more than how many
bits**, and that the only way to know is to run it.

### One detail that would have been invisible

QAT parses slightly worse on E2B -- 992 of 1001 against UD's 1001 -- while
scoring substantially better. A quant comparison tracking F1 alone shows a clean
win and hides that the winner is marginally less well-formed. Same blind spot as
every other pathology this campaign turned up.

## 31. QAT at 3002 notes, and the tier-consistency check that failed usefully

Finding 30 measured QAT against UD at n=1001 and left one thing unresolved: E4B
scored *below* E2B under QAT, -0.0213 with 95% CI [-0.0445, +0.0015], which is
backwards for a model whose smaller sibling is a nested submodel of it. The mid
tier was run to settle that, and the E2B side is banked.

### The headline: the tier does not move the score

| arm | n | strict F1 | precision | recall |
|---|---:|---:|---:|---:|
| E2B QAT, native gold_small | 1001 | 0.6406 | 0.6294 | 0.6523 |
| E2B QAT, gold_mid | 3002 | **0.6416** | 0.6276 | 0.6563 |

+0.0010 across a threefold increase in corpus size. These are different note
sets, so this is not a paired test -- it says only that the model performs the
same on the wider corpus, which is the reassuring and boring result. The value of
the mid tier is the interval it buys: roughly +/-0.014 against +/-0.024 at n=1001.

### The check that was designed in, and what it caught

Because gold_small is a strict subset of gold_mid, the 1001-note arm should be
reproduced inside the 3002-note arm. It is not:

| | strict F1 | tp | fp | fn |
|---|---:|---:|---:|---:|
| native 1001 | 0.6406 | 574 | 338 | 306 |
| the same 1001 notes extracted from the 3002 arm | 0.6327 | 572 | 356 | 308 |

> extracted - native = **-0.0079, 95% CI [-0.0278, +0.0114]**, 20000 replicates
> -- INDISTINGUISHABLE

The score is unmoved. The *outputs* are not: 529 of 1001 completions are
byte-identical, so **47% of notes produced different text** for the same model,
quant, card, process count and prompt. The only difference is which corpus the
note was embedded in. Full treatment in defect 40, including the fact that the
obvious explanation -- the immediately preceding note -- does not survive its own
test (44.8% churn with the same predecessor, 48.3% with a different one).

### Why this belongs in the QAT material rather than only in the defect log

It is the third instance of the same shape in this campaign, and by now that is
the finding rather than an incident. MTP perturbs 26% of outputs and moves F1 by
less than 0.004. Corpus composition perturbs 47% of outputs and moves F1 by
0.008, indistinguishable from zero. Quantisation-aware training changes *how the
weights were trained* and moves E2B by +0.039 while doing nothing measurable to
E4B.

**F1 is nearly blind to how the text was produced and sensitive to something
else.** The interventions that visibly rewrite half the output are the ones that
do not matter; the one that matters is invisible at the token level. Any quant
comparison reported as a single number is reporting the least sensitive
instrument available.

### The caveat this creates

`results/subset-1001/` ranks natively-run 1001-note arms -- LFM2.5, the
newcomers -- against a field extracted from banked 10k arms. Those two kinds of
number are now known to be non-equivalent. At -0.0079 the effect is well inside
the interval n=1001 supports, so no ranking conclusion in this campaign changes,
but the tables carry the caveat and it should be stated where they appear.

### The resolution: E4B does not lose to its own submodel

E4B QAT at 3002 landed at strict F1 0.6374 against E2B's 0.6416. Both arms on the
same 3002 notes, same card, same quant, nproc=1, no MTP:

| | E2B | E4B |
|---|---:|---:|
| strict F1 | 0.6416 | 0.6374 |
| tp / fp / fn | 1734 / 1029 / 908 | 1800 / **1206** / 842 |
| parse_ok = schema_ok | 2962/3002 | **3002/3002** |
| empty raw | 1 | 0 |
| rows at context limit | 1 | 0 |
| median completion tokens | 512 | 391 |
| abstention | 875 | 865 |
| reasoned | 3002/3002 | **2523/3002** |
| tool-call envelope | 0 | 0 |

> E4B - E2B = **-0.0042, 95% CI [-0.0173, +0.0088]** -- INDISTINGUISHABLE,
> 20000 replicates

**The n=1001 anomaly was noise.** The -0.0213 that looked like a model losing to
its own nested submodel shrank to -0.0042 once the interval tightened, and the
new interval excludes the old point estimate. Nothing needs explaining, because
there is no longer anything anomalous to explain. This is what defect 39 predicts
happens to a marginal result when it is finally given enough notes.

### Correcting for the floor, three ways, all of which change nothing

E2B's 0.6416 is a **floor**: 40 rows failed to parse and contributed nothing.
E4B's 0.6374 is **capability**: 3002/3002 parsed. Reporting the two side by side
without correcting is comparing a suppressed number to an unsuppressed one.

**1. Restrict both arms to the 2962 rows E2B parsed.** Paired, 20000 replicates:

| | F1 | P | R | tp/fp/fn |
|---|---:|---:|---:|---|
| E2B | 0.6434 | 0.6276 | 0.6601 | 1734/1029/893 |
| E4B | 0.6406 | 0.6038 | 0.6821 | 1792/1176/835 |

> E4B - E2B = **-0.0028, 95% CI [-0.0158, +0.0103]** -- INDISTINGUISHABLE

This conditions on one arm's failures, which is a selection effect and not
neutral in principle. Correction 2 is what bounds how much that matters.

**2. Bound the best case instead of dropping rows.** The 40 failed rows contain
**15 gold facts in total**, and 26 of the 40 have empty gold, so abstaining is
the correct answer on most of them. Perfect handling of all 40 gives tp 1734 ->
1749, fn 908 -> 893, fp unchanged, F1 **0.6454**. The entire correction is worth
at most **+0.0038** against an interval of +/-0.013.

**3. Repair the JSON.** All 40 are structurally malformed rather than wrong -- a
missing closing brace, an extra `}}` -- at a median 535 completion tokens, no
truncation flag, one row near the context limit. A lenient parser would recover
them. Not done: it means changing the instrument mid-campaign, and correction 2
already bounds the gain below the noise.

Corrections 1 and 2 move the delta in **opposite** directions -- -0.0028 and
-0.0080 against the uncorrected -0.0042 -- because one adds facts to E2B while
the other removes rows from both. Both sit inside the interval. The conclusion is
identical under all three: **E2B and E4B are the same model under QAT at this
tier.**

The general rule this arm earns: *state whether each F1 is a floor or a
capability, then bound the correction before arguing about it.* Here the bound is
a tenth of the noise, so the floor is a caveat rather than a confound. On the
arms in finding 27 it was the entire result.

### What the null is averaging over

The F1 agreement hides a real difference. E4B finds **66 more true positives and
177 more false positives** -- higher recall, materially worse precision. It
extracts more and is more often wrong. Under correction 1 the same shape holds
(recall 0.6821 vs 0.6601, precision 0.6038 vs 0.6276). Every slice of this arm
shows two models that differ in *where* their errors fall and not at all in how
many. A ranking table reports these as tied, and for choosing a model that may
even be right -- but they are not the same instrument, and a pipeline that cares
about precision should not read the tie as indifference.

### The reasoning starvation persists, and does not explain the gap

E4B skipped reasoning on 479 of 3002 rows; 204 of those answered `{"facts":[]}`
in five tokens. Those rows abstain at 51% against 24.5% on rows where it did
reason. So the partial-reasoning behaviour seen at n=1001 is not a small-sample
artefact -- it reproduces at the mid tier.

It is **not** the cause of the E4B deficit. Restricted to the 2523 rows where E4B
reasoned, E4B scores 0.6238 against E2B's 0.6420 -- a *wider* gap than on the
full corpus. E4B is worse precisely where it reasons. The obvious hypothesis is
refuted by its own test, which is the second time in this campaign that the
natural explanation for a churn pattern failed to survive being checked (the
first was the predecessor hypothesis in defect 40).

### Still open

Why E4B declines to reason on a stable ~16% of rows is unexplained. The rows are
not distinguished by context length, truncation, or the tool-call envelope. That
is a real open question, not a caveat.

### Where this goes

No article currently carries QAT. Findings 26, 30 and 31 -- the LFM2.5 quant
ladder, QAT vs UD, and this -- are one article's worth of material on the theme
that which quant matters more than how many bits, and that the measuring
instrument is less sensitive than the thing being measured.

## 32. A third of the corpus cannot raise any score, and it is where the models differ most

Article 6 open item 3 asked whether the MTP null is an average of two opposite
effects. Splitting the six paired 10k arms by note category answers that -- and
turns up something much larger on the way.

### The answer to the MTP question: no, the null is real

| pair | aggregate delta | category spread | sign split |
|---|---:|---:|---|
| E2B Q4 | +0.0039 | 0.0360 | 6 pos / 4 neg |
| E2B Q6 | +0.0013 | 0.0100 | 4 pos / 6 neg |
| E2B Q8 | -0.0022 | 0.0322 | 4 pos / 6 neg |
| E4B Q4 | -0.0005 | 0.0187 | 3 pos / 7 neg |

No category comes close to the +0.24 that the reasoning-on/off null was hiding.
The largest single movement is implicit on E2B Q8 at +0.0220, which is inside the
+/-0.024 that n=723 supports and so is not a finding. The MTP null is a null all
the way down, not a cancellation. **Article 6 open item 3 is closed.**

### What the split exposed, and the part of it I got wrong

Three categories score **exactly 0.0000 strict F1** in the first version of that
split: negation (1318), transient (1391), ambiguous (506) -- 3215 of 10000 notes,
32.1% of gold_large, all with empty gold by construction. They are abstention
tests: the correct answer is no facts.

I read that as a structural blind spot in F1 and wrote it up as one. **That was
wrong, and it was wrong against the project's own source.** Two checks settle it,
both of which should have come before the writeup:

**1. The zeros were my bug, not the scorer's.** `score.py` already emits `null`
rather than 0.0 for factless categories, with a comment giving the reason: "fp=0
on a factless note is perfect restraint, not failure, and a chart would show it
as the worst category." My `mtp_by_category.py` printed 0.0000 and thereby
reintroduced precisely the error the scorer was written to avoid. Fixed.

**2. The rows are not invisible to F1.** The scoring loop runs over every note,
so a spurious triple on a factless note lands in global fp exactly like any other
false positive. Setting the false positives on those rows to zero:

| arm | F1 | spurious | F1 with perfect abstention | gain |
|---|---:|---:|---:|---:|
| E2B Q4 | 0.6246 | 1083 | 0.6644 | +0.0398 |
| E2B Q6 | 0.6344 | 1145 | 0.6773 | +0.0429 |
| E2B Q8 | 0.6329 | 1142 | 0.6754 | +0.0425 |
| E4B Q4 | 0.6301 | 1410 | 0.6808 | +0.0507 |
| E4B Q6 | 0.6452 | 1413 | 0.6975 | +0.0523 |
| E4B Q8 | 0.6337 | 1456 | 0.6865 | +0.0528 |

Over-extraction on the abstention third is worth **0.040 to 0.053 F1** -- an
order of magnitude larger than every effect this campaign has argued about. The
corpus design is sound and the metric responds to it. What I published as "a
third of the corpus can only cost points and correct restraint is worth exactly
nothing" is withdrawn: restraint is worth about four to five F1 points, which is
what a precision term is supposed to do.

**3. The metric I "introduced" has been in every score.json all along.** Each one
carries an `over_extraction` block: `abstention_rate_on_schema` and
`spurious_triples`, with a comment explaining that abstention is credited only
where the model emitted the right shape, so a broken model cannot look maximally
precise. E2B Q4 reads 0.674 and E4B Q4 reads 0.578. `abstention_quality.py`
rediscovered a number that was already on disk, which is the fourth time this
campaign has "found" something it had already measured.

### What actually survives

The behavioural difference is real, and it is the part worth keeping:

| arm | abstention rate | spurious triples |
|---|---:|---:|
| E2B Q4 / Q6 / Q8 | 0.674 / 0.658 / 0.659 | 1083 / 1145 / 1142 |
| E4B Q4 / Q6 / Q8 | **0.578 / 0.576 / 0.565** | 1410 / 1413 / 1456 |

E4B stays silent on 57-58% of factless notes; E2B on 66-67%. Stable across all
three quants, same card, same process count, same tier -- no confound between
these six arms.

And F1 does **not** hide this, it *nets* it. At Q4, E4B leads E2B by +0.0055 as
scored; with both abstaining perfectly the lead would be +0.0163. E4B is the
stronger extractor and pays roughly a point of that back in over-extraction. The
single number is the sum of two opposed movements, which is what a single number
is for -- but a reader choosing a model for a pipeline that cannot tolerate
invented facts needs the parts, and the parts were sitting in the score files
unread.

### The MTP null survives on this metric too

MTP against no-MTP on the same rows moves the invention rate by 3 to 23 rows out
of 3215 (E2B Q4 31.8/31.9, Q6 33.5/34.0, Q8 34.0/34.5, E4B Q4 45.1/44.4). So
speculative decoding does not trade restraint for speed either. That is a
stronger statement than the F1 null, because it is measured on the rows F1
cannot see.

### One contrast that is NOT clean, and is flagged rather than explained

Under QAT at nproc=1 on the 5080, the gap disappears: E2B 277/965 (28.7%), E4B
282/965 (29.2%) on gold_mid. Four things differ from the arms above at once --
quant scheme, process count, card, tier -- so this comparison carries no weight
in either direction. It is recorded because it is the one place the E4B
abstention deficit does not appear, and finding out which of those four variables
is responsible is a real open question.

### For the articles

The keepable claim is narrow: **report abstention rate beside F1.** Not because
F1 is blind to over-extraction -- it is not, it prices it at four to five points
-- but because one number is the net of two opposed behaviours, and two models
that tie can differ by ten points in how often they invent facts. `score.py`
already computes it; the campaign simply never printed it.

The retraction is the more useful story. This was the sixth "F1 is blind to X"
finding, the shape was familiar, the data fit, and it took two checks against the
project's own source to notice that the scorer had handled the case deliberately
and documented why. Pattern-matching to a house theme is its own failure mode,
and it is faster than the checks that catch it.

## 33. The head-to-head is mostly not a ranking

`harness/h2h_intervals.py` runs a paired bootstrap on every adjacent pair in the
head-to-head table, because an adjacent pair IS the ordering claim a ranked table
makes. Eight thousand replicates, gold_small, n=1001.

**Nine of fifteen adjacent pairs are indistinguishable.**

| pair | delta | 95% CI | |
|---|---:|---|---|
| E2B-QAT vs E4B-QAT | -0.0213 | [-0.0442,+0.0017] | indistinguishable |
| E4B-QAT vs E4B-UD | -0.0027 | [-0.0225,+0.0174] | indistinguishable |
| E4B-UD vs E2B-UD | -0.0150 | [-0.0391,+0.0085] | indistinguishable |
| E2B-UD vs LFM2.5-2.6B-Q4 | -0.0163 | [-0.0468,+0.0150] | indistinguishable |
| LFM2.5-2.6B Q4 vs Q8 | -0.0104 | [-0.0366,+0.0153] | indistinguishable |
| LFM2.5-2.6B-Q8 vs granite-4.1-3b | -0.0275 | [-0.0575,+0.0028] | indistinguishable |
| granite-4.1-3b vs gemma-3n-E4B | -0.0144 | [-0.0461,+0.0166] | indistinguishable |
| gemma-3n-E4B vs LFM2.5-8B-A1B | -0.0133 | [-0.0441,+0.0177] | indistinguishable |
| LFM2.5-8B-A1B vs Qwen3-1.7B | -0.0580 | [-0.0912,-0.0247] | **separable** |
| Qwen3-1.7B vs SmolLM3-Q8 | -0.0685 | [-0.1027,-0.0345] | **separable** |
| SmolLM3-Q8 vs granite-4.0-1b | -0.0022 | [-0.0344,+0.0312] | indistinguishable |
| granite-4.0-1b vs SmolLM3-Q4 | -0.0330 | [-0.0662,-0.0002] | **separable** |
| SmolLM3-Q4 vs LFM2.5-VL-1.6B | -0.0856 | [-0.1145,-0.0567] | **separable** |
| LFM2.5-VL-1.6B vs LFM2.5-1.2B | -0.1054 | [-0.1375,-0.0735] | **separable** |
| LFM2.5-1.2B vs LFM2.5-230M | -0.0362 | [-0.0650,-0.0065] | **separable** |

The top eight rows of the table are one group. Nothing in them is ordered by this
data. The separable pairs are all in the bottom half, where the gaps are large
because the models are failing rather than competing.

**A ranked table is a claim per row boundary, and most of mine are unsupported.**
Printing models in F1 order implies an ordering the interval does not carry. The
honest presentation is tiers, not ranks.

### One published claim does not survive

| claim | delta | 95% CI | |
|---|---:|---|---|
| QAT beats UD on gemma-4-E2B | -0.0390 | [-0.0635,-0.0152] | **holds** |
| more bits helps SmolLM3-3B | -0.0351 | [-0.0543,-0.0165] | **holds** |
| E2B-QAT beats E4B-UD | -0.0240 | [-0.0445,-0.0034] | **holds** |
| **fewer bits helps LFM2.5-2.6B** | **-0.0104** | **[-0.0366,+0.0153]** | **WITHDRAWN** |
| granite-4.1-3b vs the arm above it | -0.0144 | [-0.0461,+0.0166] | indistinguishable |

The LFM2.5-2.6B inverse ladder was one of three ladder shapes carrying the "more
bits is not a direction" argument. It is indistinguishable from zero and comes
out. The argument survives on the two that hold, and it survives BETTER for
losing the third: SmolLM3 rising and gemma-4 E4B falling at Q8 are enough, and
they are measured rather than asserted.

**What this does not cost.** The restraint finding is untouched, because it was
never an F1 claim: granite-4.1-3b invents on 71 factless notes against
LFM2.5-VL-1.6B's 279, and abstention rates are 0.786 against 0.171. Those gaps
are not close to their intervals. The piece's central argument, that F1 and
restraint are independent axes, is the part that survives an interval sweep
intact while the ranking around it dissolves.

## 34. Speculative decoding pays the same on QAT and post-hoc weights (ANSWERED: no effect)

### The observation, and why it is not yet evidence

Two rented arms, both gemma-4-12B, both UD-Q4_K_XL, same corpus, same client:

| arm | med latency | tok/s |
|---|---:|---:|
| non-QAT weights, MTP on | 6267 ms | **102.9** |
| QAT weights, MTP off | 7737 ms | 67.2 |

+53% tok/s. **That comparison is worthless as it stands** and is recorded only to
say why. It varies three things at once: MTP on against off, QAT weights against
post-hoc, and two different rented hosts. It also rests on 13 rows for the faster
arm. Any of the three could produce the whole difference.

### The question underneath it, which is real

Speculative decoding's speedup is a function of **draft acceptance rate**: the
fraction of drafted tokens the target model accepts as its own argmax. The draft
head is trained against the base model. Quantisation-aware training changes the
target's weights in a way post-hoc quantisation does not, so the target's output
distribution moves relative to what the draft learned to predict.

If that shift is material, **QAT should get less benefit from MTP than a post-hoc
quant of the same model does**, because a lower acceptance rate means more
verification passes discarded. If it is immaterial, the speedup should be
indifferent to the quant scheme.

Either answer is worth having. The whole MTP result in this project rests on
"roughly a doubling", measured only on non-QAT UD quants of E2B and E4B, and the
head-to-head's best model is a QAT build. If the speedup does not transfer to
QAT, the recommendation to use both together is weaker than it currently reads.

### The design, which the current fleet half-satisfies

A clean answer needs a 2x2 per model size: {QAT, non-QAT} x {MTP, no MTP}, all
four arms on the same corpus at nproc=1, with the SAME draft file across the two
MTP arms so the draft is not itself a variable.

Running now: the two MTP cells at 12B and at 31B. **The two no-MTP cells at each
size are missing** -- I killed them when switching the fleet to MTP, which was
right for throughput and wrong for this question, and I did not notice at the
time that it destroyed a 2x2 I had not realised I was holding.

Cost to complete: four arms, roughly $0.3 at observed rented prices.

### What to measure beyond wall clock

Draft acceptance rate directly, if llama.cpp exposes it per request. Latency and
tok/s are downstream of it and confound it with everything else about the host.
`/props` and the server log should be checked for an acceptance counter before
the arms are re-run, because measuring the mechanism is worth more than measuring
its shadow.

### Where it goes

Draft 01 (speculative decoding) currently says the speedup is "a property of the
model and the backend, not of the feature", supported by 1.59x for E2B against
1.83x for E4B. If the quant scheme is a third term, that sentence needs a third
clause and the article gets a genuinely new section. Draft 02 (quant) gains the
converse: a reason to care which quant you pick that has nothing to do with
accuracy.

### PROVENANCE WARNING: the source files for this table were deleted

The acceptance figures below came from two partial arms (31 and 24 rows) whose
prediction files I deleted during a fleet relaunch, with `rm -f
results/vast/*.mtp.live.pred.jsonl`. They had never been committed, so they are
not recoverable. The numbers are recorded here and nowhere else, which is exactly
the condition this project treats as unacceptable: a figure that cannot be traced
to an artifact.

The deletion bought nothing. `run_llamacpp.py` truncates its output on start, so
leaving the files would have been harmless.

**Treat the table below as provisional until re-derived from the completed arms.**
Both arms are re-running at full n=1001, which will give roughly thirty times the
drafted tokens these figures rest on. If the re-derived numbers differ, the
re-derived numbers are the ones that count.

### ANSWER: no. Acceptance is indistinguishable, and the throughput gap was the host.

gemma-4-12B, same draft file, same corpus, same prompt, MTP on both sides:

| arm | drafted | accepted | acceptance |
|---|---:|---:|---:|
| QAT UD-Q4_K_XL | 30,328 | 24,913 | **82.1%** |
| non-QAT UD-Q4_K_XL | 33,452 | 27,627 | **82.6%** |

Half a point apart on more than sixty thousand drafted tokens. **The hypothesis
is refuted.** Quantisation-aware training does not move the target's output
distribution far enough from what the draft head predicts to cost acceptance, so
the MTP speedup transfers to QAT builds intact.

That matters for the recommendation rather than for the theory: the head-to-head's
best model is a QAT build, and article 01's speedup was measured only on non-QAT
UD quants. Those two results now compose instead of sitting next to each other
with an untested gap between them.

### The number that looked huge, and was the host

Wall-clock throughput for the non-QAT arm, across five placements in one hour,
same model, same quant, same draft, only the rented machine differing:

    131.9 -> 100.7 -> 109.6 -> 101.6 -> 84.4 tok/s

A 1.5x spread with nothing about the model changing. Set beside the QAT arm's
163.7 tok/s it reads as a large quant effect and is not one. This is exactly why
`draft_n`/`draft_n_accepted` were added: **tok/s prices the mechanism and the
machine together, and on rented hardware the machine dominates.**

Every MTP figure in this project before today is wall clock, including the
+84/92/102/111/116% ladder. Those were all measured on ONE card with everything
else held, so they are not contaminated in the way these rented numbers are. But
they measure the shadow, and the acceptance rate measures the thing.

### What to write, and what not to

**Write:** MTP and QAT compose. Acceptance is ~82% on both, so a QAT build gets
the same speedup a post-hoc quant does, and the two cheapest wins in this project
stack.

**Do not write:** that QAT changes MTP throughput. The evidence for that is a
tok/s comparison across different rented machines, and the same arm varies by 1.5x
across machines on its own.

## 35. The six-pair MTP ladder is complete, and the null holds across both families

Six paired 10,000-note arms, one card (RX 7900 XTX), three processes, the draft
model the only difference between the two sides of each pair.

| model | quant | MTP | no-MTP | ΔF1 | steady throughput |
|---|---|---:|---:|---:|---:|
| E2B | Q4 | 0.6246 | 0.6207 | +0.0039 | +84.0% |
| E2B | Q6 | 0.6344 | 0.6331 | +0.0013 | +91.6% |
| E2B | Q8 | 0.6329 | 0.6351 | −0.0022 | +102.5% |
| E4B | Q4 | 0.6301 | 0.6306 | −0.0005 | +110.6% |
| E4B | Q6 | 0.6452 | 0.6435 | +0.0017 | +116.2% |
| **E4B** | **Q8** | **0.6337** | **0.6327** | **+0.0010** | **+131.3%** |

**Sign flips three times. Largest |ΔF1| is 0.0039.** Two pairs have paired
bootstraps at 20,000 replicates over the same 10,000 notes:

> E4B Q4: no-MTP − MTP = +0.0005, 95% CI [−0.0028, +0.0036]
> E4B Q6: no-MTP − MTP = −0.0017, 95% CI [−0.0048, +0.0013]
> E4B Q8: no-MTP − MTP = −0.0010, 95% CI [−0.0041, +0.0021]

Three precise nulls, each bounded inside ±0.005. Not "we cannot tell" -- *the
effect is smaller than five thousandths of an F1 point in either direction*.

### Throughput climbs monotonically and the accuracy does not move

84.0 → 91.6 → 102.5 → 110.6 → 116.2 → **131.3%**. The gain rises with quant size
within each family, which is what bandwidth-bound decode predicts: a bigger target
model spends more time waiting on memory, so there is more idle compute for
speculation to reclaim. Q8 gains most because it is the heaviest to read.

### The final pair is capability on both sides

| | MTP | no-MTP |
|---|---:|---:|
| parse_ok = schema_ok | 10000/10000 | 10000/10000 |
| empty raw / at context | 0 / 0 | 0 / 0 |
| median completion tokens | 354 | 353 |
| abstention | 2058 | 2076 |
| invented on factless rows | 1418 (44.1%) | 1399 (43.5%) |
| reasoned | 9994 | 9993 |
| tool-call envelope | 0 | 0 |

Nothing is suppressed on either side, so both figures are capability rather than
floors. Abstention differs by 19 rows in 3,215 and invention by 19 triples, so
speculative decoding does not trade restraint for speed either -- a stronger
statement than the F1 null, because it is measured on the third of the corpus F1
prices only through false positives.

### What the ladder now supports

**Turn it on.** Six pairs, both model families, three quant levels each: roughly a
doubling of throughput, rising to 2.3x at Q8, at an accuracy cost bounded inside
±0.005 F1 by three independent bootstraps.

The limits are unchanged and load-bearing. It is repeatable but NOT identical
(74/100 against a sequential arm, 100/100 against itself), so an MTP arm cannot be
compared to a non-MTP arm. And the speedup belongs to the model and the backend
rather than to the feature.

One limit is now weaker than it was. Article 01 says "I can only vouch for
gemma-4, the only family here that publishes an MTP draft". That is no longer
true: Qwen3.6 ships MTP layers in the model file rather than as a separate draft,
and 12B, 26B-A4B and 31B gemma-4 builds all publish drafts. The rented arms
running tonight extend this beyond E2B and E4B for the first time.

## 36. QAT is genuinely faster, and the rented 1.7x was mostly the host

Both gemma-4-12B builds on ONE card (RTX 5080, LXC 140), same binary, same
context, same cache setting, same probe text, same MTP draft family, six identical
requests each. Quant scheme is the only difference.

| build | file | tok/s median | spread | draft acceptance |
|---|---:|---:|---|---:|
| QAT UD-Q4_K_XL | 6.26 GiB | **285.7** | 294 285 285 286 285 286 | 85.4% |
| non-QAT UD-Q4_K_XL | 6.86 GiB | **233.1** | 234 234 233 232 233 233 | 84.2% |

**QAT is 22.6% faster.** Not the 1.7x seen across rented machines, and not zero.

### What this settles

The rented comparison gave QAT ~232 against non-QAT ~137 tok/s, a 1.7x gap. On one
card the gap is 1.23x. **So roughly two thirds of the rented difference was the
machine and one third was the model.** Either number alone would have been wrong:
reporting 1.7x would have credited the quant with the host's contribution, and
dismissing it as "host variance" (which is what I did at 21:43) would have thrown
away a real 22.6%.

The mechanism is not speculation. Acceptance is 85.4% against 84.2%, a 1.2 point
difference in the wrong direction to explain a 22.6% speed gap. It is also not
purely file size: 6.26 against 6.86 GiB is 8.7% less data per token, which
predicts roughly 8.7% more throughput for bandwidth-bound decode, not 22.6%.

The residual is consistent with a different tensor-type mix under the same
UD-Q4_K_XL label. unsloth assigns bit widths per tensor by sensitivity, and
sensitivity analysis on QAT weights can select cheaper types; i-quant tensors
dequantise more slowly than K-quants. **That is consistent-with, not measured.**
Confirming it needs a tensor-type dump of both files, which nothing here has done.

### Why the numbers are trustworthy this time

Six probes per build, spread under 3% on both. Every earlier throughput comparison
in this project came from rented boxes where the same arm varied 84 to 232 tok/s
across placements. A dedicated card removes that entirely, and the tightness of
the spread is the evidence that it did.

### For the articles

articles/v2/02 currently recommends QAT on accuracy alone (+0.0389 F1 on E2B, 95%
CI [+0.0152, +0.0635]). It now has a second, independent reason: on gemma-4-12B
the QAT build is **22.6% faster on identical hardware**, and it is smaller on disk.
Better, faster and smaller is rare enough to state plainly.

The caveat that stays: this is one model at one size. E2B showed +0.0389 accuracy
and E4B showed none, so the accuracy benefit does not generalise across sizes, and
there is no reason yet to think the speed benefit does either.
