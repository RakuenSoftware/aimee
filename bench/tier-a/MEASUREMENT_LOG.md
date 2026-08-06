# Measurement log: every scoring bug, and how it was found

A running record of the defects found in this benchmark's **own instrumentation**,
kept because it is the most transferable thing here. The models behaved roughly
as expected throughout. The grader did not.

Fourteen defects. Most inflated the apparent failure rate, one deflated it, two
distorted the ranking rather than the level, several inverted a conclusion, one
overturned the central result of the entire exercise, and none were in the
models.

---

## The pattern

Eight of the nine share one shape: **the metric punished a correct answer
expressed differently from the label.** Once that pattern was named it became a
search strategy — look for places where "different" was being scored as "wrong" —
and it kept paying out.

The ninth (incomplete gold) is worse and subtler: the metric punished answers
that were correct and *not labelled at all*, which penalises the models that
extract most. That one biases the ranking rather than just depressing it.

---

## 1. Symmetric relations scored as wrong

**Found by** hand-reading E4B's disagreements after its score looked low.

`rel_type_def_t` carries `is_symmetric`, and the C comment states "one assertion
implies both directions". `knows` and `spouse` are symmetric, so `(sarah, spouse,
user)` is correct — but the scorer charged it twice, as a false positive *and* a
false negative.

**Impact** E4B 0.623 → 0.656. **Lesson** the ontology already encoded the answer;
the scorer just wasn't reading it.

## 2. Inverse relations scored as wrong

**Found by** suspecting the same class of bug and grepping the ontology for other
metadata the scorer ignored.

`inverse_rel_type` is documented "auto-enforced": asserting `(a parent_of b)`
commits `(b child_of a)`. The scorer treated the two directions as different
facts.

**Impact** small alone, but it confirmed the pattern was systematic.

## 3. Number words scored as wrong

`"Nina is seven"` → a model writing `7` and a label saying `seven` are the same
scalar to the ontology. Scored as a miss.

## 4. Fabrication metric raising false alarms

**Found by** validating a brand-new metric before trusting it — the one time
checking happened *before* publishing rather than after.

The grounding check flagged `kb_server` against "KB server", `7` against
"seven", and `me` as invented entities. **Qwen3.5-0.8B appeared to fabricate
11.4% of its triples; the true figure is 0.0%.**

**Lesson** a new metric deserves the same suspicion as a new model.

## 5. Schema validity — the one that inverted a conclusion

**Found by** an observation from outside: schema validity fell monotonically with
model size (1.00 → 0.96 → 0.84 → 0.77 across E4B, 12B, 26B, 35B), which is a
strange thing for bigger models to be worse at.

All 30 "schema failures" across all four models were the literal string `{}` or
`[]`, on notes asserting no durable fact. **Zero were malformed.** The models
were abstaining correctly and saying so tersely; the runner recorded anything
without a `facts` array as a failure. Production agreed with the models, not the
metric — `mf_commit_facts()` commits nothing either way.

**Impact** all four models are 1.00. It also excluded those notes from the
abstention denominator, understating abstention for exactly the models that
abstained most.

**What it cost** an entire published finding. A "dense models are more
disciplined than MoE" result, complete with a plausible mechanism (per-token
expert routing degrading format adherence), was **retracted**. The mechanism was
a story fitted to a bug.

## 6. Entity normalisation

`kb_server` ≠ `KB server`, `aimee-kb` ≠ `aimee kb`, `Dr. Okafor` ≠ `Dr Okafor`.
Endpoints had to match exactly under a normaliser that preserved underscores and
hyphens. Models write snake_case constantly.

**Impact** narrow but real: only Qwen3.5-0.8B moved, +0.040.

## 7. Incomplete gold — the largest single error

**Found by** auditing all 220 distinct false positives across 20 models, after
being told that finding one bug this way justified checking the rest.

Notes contained durable facts that were never labelled, so correct extractions
scored as false positives. `"That box's hostname is aimee-retrieval-test"` was
labelled EMPTY and **nine independent models** extracted the hostname. The models
were right.

**Impact** +0.02 to +0.10 F1 for every model. The incumbent went 0.705 → 0.754.

**Why it is the worst kind** it is *correlated with model quality*. Better models
extract more, so they collect more unlabelled-but-correct triples, so they are
penalised more. A metric that is merely noisy is survivable; one that is biased
against the thing you are trying to measure is not.

**Fix** seven facts promoted to required gold (64 → 72 triples). Where two
renderings are equally correct — naming a device by description rather than
hostname — an `alt` attaches to the specific gold triple and scores as a true
positive, rather than being excused from the denominator.

## 8. Equivalent predicates

`speaks` vs `speaks_language`, `attends` vs `member_of`, `daughter` vs
`child_of` — each cost a model both a false positive and a false negative for a
naming choice carrying no information.

Handled by an explicit equivalence table, kept deliberately narrow: `studied_at`
is **not** equivalent to `studied`, because "studied medicine" and "studied at
Otago" are different facts. Bare kinship terms were also missing from the
*production* alias table, so this one found a real product defect too.

## 9. Object containment

`"2 of the junior engineers"` for `"junior engineers"`, `"proxmox host in the
auckland rack"` for `"auckland rack"` — token-F1 0.5 and 0.571, just under the
0.6 threshold, despite the gold being fully *contained* in the prediction.

---

## Checks that came back clean

Worth recording, because "we looked and found nothing" is evidence too:

- **Greedy vs optimal matching.** Greedy 1-1 assignment could in principle
  undercount when one prediction is the only match for two gold triples. Checked
  against maximum bipartite matching for every model: **identical on all**.
- **Arithmetic invariants.** `tp+fn` equals the gold total and `tp+fp` equals the
  prediction total for every model.
- **False negatives.** Audited the same way as false positives. Unlike the FPs,
  these were overwhelmingly *genuine model errors* — `lives_in` where the note
  said `born_in`, `works_for` for colleagues. One gold fix came out of it.

## Guards now in place

Each defect left behind something that fails loudly rather than a promise to be
careful:

| Guard | Prevents |
|---|---|
| `prompt.verify_against_source()` | benchmark prompt drifting from the C source |
| `score.py` row-count check | a partial run scoring as a bad result (a 1-note remnant once scored F1 0.031 for a model that scores 0.903) |
| `validate_gold.py` | duplicate triples, blank fields, alternatives identical to their parent |
| `rel_types_self_validate()` | an alias pointing at a non-existent or inactive relation |
| `sync_results.sh` rescoring | the CT's stale scores silently reverting local fixes — this happened **three times** |
| `audit_errors.py`, `predicate_drift.py` | reclassifying every disagreement on demand |

## 10. Over-crediting, found by noticing all fixes moved one way

**Found by** an outside observation that something still felt wrong, after nine
fixes that had every one moved scores *up*.

Auditing the credits granted by each relaxation found two that substituted a
different entity and called it the labelled fact: `optane pool | located_in |
proxmox host` credited for a gold triple about the Auckland rack, and `user |
knows | anand's kids` credited for one about Anand.

**The rule that came out of it, which is now enforced:** an alternative may
differ ONLY in the predicate. Both endpoints must name the same entity. Surface
variation is normalisation's job; predicate variation is expected and fine —
`works_for` vs `member_of`, `born_in` vs `grew_up_in`. Renaming an endpoint is a
different node and a different claim.

**Impact** the first corrections in the exercise that moved DOWN. E4B 0.723 ->
0.692.

**A related error of framing.** I had defended endpoint leniency by noting that
aimee has entity aliasing, so `build host` and `forge` would resolve to one node
downstream. That is not a reason the extraction was correct. This benchmark
measures extraction, not aimee's pipeline — and that aliasing code exists to
compensate for an imperfect embedder, not to license loose scoring. **Strict is
the headline metric**: both endpoints exact, predicate flexible, no assumption of
a resolution step that is not being measured.

## 11. Per-model error audits: predicate coverage, containment in strict

**Found by** repeating the best-model error audit across every model, and
aggregating the disagreements that recurred.

Three more:

- **Containment belonged in strict too.** "2 of the junior engineers" for a gold
  of "junior engineers", "clinical director at st vincent's" for "clinical
  director". In each case the model was MORE faithful to the note than the label
  — the note says "mentors two of the junior engineers". Strict was penalising
  models for being more specific than an under-specified gold.
- **Predicate equivalence was too thin.** Models write `joined` and
  `board_member` for `member_of`, `profession` for `has_role`, `met` for `knows`,
  `started_at` for attending, `located_in` for `lives_in`. Endpoints identical,
  predicate different — which the rules already permit; the table just did not
  cover them.
- **Honorifics and converse predicates**, above.

**Impact, and the useful part:** the leaders barely moved (26B unchanged at
0.920, 12B unchanged at 0.855) while the smaller models gained 0.02-0.05.
Weaker models reach for non-canonical predicates more often, so a thin
equivalence table was penalising them specifically. That is a *rank-distorting*
bias, not a level shift — the same defect class as the incomplete gold, pointing
the other way.

Every credit granted by the new rules was audited individually. All legitimate;
no over-crediting.

## 12. The confidence floor was doing the measuring — and it overturned the headline

**Found by** checking two harness parameters that had never been validated: the
token cap and the confidence floor. The cap was fine (only LFM2's repetition loop
reaches it; every other model peaks at 254 tokens against a 512 limit). The floor
was not.

`MF_CONF_FLOOR` (0.6) discards any fact below the threshold. Scoring with it
applied conflates two different things — whether a model can extract the fact,
and whether it emits a usable confidence — and the conflation falls almost
entirely on small models:

| model | capability | committed | dropped |
|---|---:|---:|---:|
| Qwen3-0.6B | **0.400** | **0.000** | 72 of 73 |
| granite-4.0-350m | 0.205 | 0.000 | 53 |
| granite-4.0-h-350m | 0.135 | 0.000 | 20 |
| Qwen3-1.7B | 0.563 | 0.396 | 44 |
| granite-4.1-3b | 0.643 | 0.567 | 13 |
| gemma-4-26B-A4B | 0.913 | 0.920 | 1 |
| gemma-4-12B | 0.855 | 0.855 | 0 |

**What it cost: the central conclusion.** "Nothing at or below 600M produces a
usable fact" was reported repeatedly across this whole exercise. It is false.
Qwen3-0.6B extracts at 0.400 F1 and every one of those facts is correct — the
floor throws all 72 away because the model writes `confidence: 0.0`.

The floor is a config value we choose, not a property of any model. The top four
models are unaffected by it (±0.011), so it was invisible in exactly the place I
was looking hardest.

**The subtlety worth keeping:** it is not a uniform penalty. gemma-4-E2B scores
*lower* without the floor (0.641 -> 0.575), so for that model the floor works as
designed, filtering low-confidence noise. It is a precision filter that some
models benefit from and others are destroyed by.

Both numbers are now reported: **capability** (floor lifted — can the model do
the task) and **committed** (floor applied — what the drain writes today). The
gap between them is a config decision, and for the small models it is the whole
story.

## 13. Correctness pass: bad gold found by consensus, and the scorer finally tested

Two angles not tried before.

**Gold triples no model produces.** If 23 models all miss the same labelled fact,
the label is the likelier error. Exactly one triple qualified:
`am05 ingrid|has_role|manager` from "My manager's manager is Ingrid." The models
produced ten different readings, none more than twice, none matching the label —
Ingrid is not "a manager" in the has_role sense, she is two levels up a reporting
chain whose intermediate entity is unresolved, and the ontology has no reporting
relation. Now empty, alongside the other ambiguous items.

That every other gold triple was produced by at least three models is the
reassuring half of this check.

**Unit tests for the scorer.** Every check until now audited its output on real
predictions, which finds bugs only where a model happened to trip one. Sixteen
constructed cases with arithmetic answers now cover: exact match, empty
prediction, one spurious, one missed, symmetric swap, inverse direction, an
asymmetric swap that must NOT be credited, endpoint renaming that must not be
credited, surface variation that must be free, equivalent and unrelated
predicates, duplicates, factless notes, a terse `{}` counting as abstention, both
confidence views, and refusal of an incomplete file.

All pass. The single failure was in a test assertion, not the scorer — comparing
at 1e-6 against figures the scorer rounds to 4dp.

That these pass does not retroactively validate the twelve defects above; most
were wrong *labels* or wrong *rules*, which arithmetic tests cannot catch. What
the tests do is stop the rules regressing now that they are right.

## 14. Scorer deep dive: properties, not outputs

Previous passes audited what the scorer *said* about real predictions. This one
attacked the scorer itself.

**Clean:**
- *Order invariance.* Greedy matching iterates gold and predictions in list
  order, so it could in principle depend on that order. Twelve shuffles per
  model: identical F1 every time.
- *Monotonicity of the relaxations.* Each rule may only ADD matches, never
  remove one. Checked per note and per model against exact-match-only:
  **zero violations**. Worth knowing how much the rules carry — they add 3 to 11
  true positives per model, and granite-4.0-h-1b gains 46% (24 -> 35). Rules
  doing that much work had better be tested, which until now they were not.

**Found:**
- *The completeness guard was in the wrong place.* It lived in score.py's
  main(), so every ad-hoc analysis importing the module skipped it — an
  unfinished 31B run scored 0.527 in the order-invariance check before I noticed
  it was 49 notes of 70. Now a shared `load_pred_file()` that refuses.
- *Endpoint asymmetry.* Containment applied to objects but not subjects, an
  accident of where the failing cases happened to appear. Zero impact on current
  data, but it would have produced a surprising result the first time a model
  elaborated a subject the way they routinely elaborate objects. Made symmetric;
  no score changed.
- *Two normalisation bugs, from feeding it malformed input.* An entity
  legitimately named "a" normalised to the empty string, because article
  stripping ran unconditionally. And `ground_text(None)` produced the literal
  string "none", which can match a note containing that word. Both fixed, both
  now regression-tested.

**The pattern worth naming:** every one of these was found by asking what the
scorer must be *true of*, rather than by looking at what it produced. Output
audits find the bugs your data happens to trip. Property checks find the ones it
does not — yet.

## Known gotchas that are NOT fixed

Things a reader or a rerun will hit. None are bugs; all are limits.

**The scoring rules were fitted to this data.** This is the biggest one and it
has no clean fix here. Predicate equivalences, aliases and containment were added
*because models produced them* — I read the disagreements and decided which were
unfair. Measured against exact-match-only, those rules account for **6-13% of
every top model's F1**:

| model | exact only | with rules | attributable to rules |
|---|---:|---:|---:|
| gemma-4-26B-A4B | 0.809 | 0.926 | 13% |
| gemma-4-12B | 0.769 | 0.862 | 11% |
| gemma-4-E4B | 0.656 | 0.738 | 11% |
| Qwen3.6-27B | 0.820 | 0.906 | 10% |
| Qwen3.6-35B-A3B | 0.746 | 0.791 | 6% |

Each rule is individually defensible — `met` really does mean `knows`. But they
were chosen with the answers visible, which is the definition of fitting the
grader to the test set. The proper control is a held-out set of notes the rules
were never tuned against; it does not exist here. Anyone reproducing this should
treat the absolute numbers as generous by roughly a tenth, and note that the
effect is uneven (6% to 13%), so it distorts gaps as well as levels.

**One author, no independent review.** Every gold label was written by one model
(Claude Opus 5) and audited by the same one. The consensus check in defect 13
helps — 23 models agreeing against a label is real evidence — but it cannot catch
a mistake the models share.

**n=69.** Differences under about 0.05 F1 are not meaningful. Several adjacent
pairs in the table are inside that.

**Categories with no gold triples report null, not 0.** transient, and most of
ambiguous and negation, have no gold triples, so P/R/F1 are undefined there.
Reporting 0.0 inverted the meaning — fp=0 on a factless note is perfect
restraint, and a chart would have shown it as the worst category. Read
`abstention_rate_on_schema` for those instead.

**Latency is not comparable across runs.** Offloaded GPU figures (12B, the MoEs
under transformers) are an artefact of paging, not model speed. CPU figures share
a host with other containers. Only the llama.cpp resident runs are worth quoting.

**Weights were pruned after scoring.** Model repos are mutable; `PROVENANCE.json`
pins the revision SHAs, and a rerun that resolves a different revision is not
running the same experiment.

## What this cost, and what it is worth

Five of the nine were found only after a number had been reported. Three
conclusions were retracted: the MoE discipline gap, "over-extraction is inherent
to this prompt", and an 11.4% fabrication rate.

The uncomfortable part is that for the first nine, **every single correction
moved in the same direction** — the models were better than measured, every
time. A grader with unbiased noise would have erred both ways.

That one-sidedness was itself the clue, and it is what prompted the tenth: if
every fix helps the models, the fixes are probably overshooting somewhere. They
were. Looking specifically for over-crediting found it immediately. The lesson is
not "audit your grader" but something narrower and more useful: **audit it in the
direction your corrections have not been going.**

The practical lesson for anyone building an eval: the failure mode is not models
behaving unexpectedly. It is the grader being wrong in ways that correlate with
model quality — and a benchmark cannot detect that about itself. Every defect
here was found by reading raw outputs or by an outside observation that a number
looked strange. None were found by the aggregate metrics, which looked entirely
plausible throughout.

## Defect 15: results/gpu/ contained CPU runs

`sweep_b.sh gpu` and `sweep.sh`'s llamacpp arm write to a directory named `gpu`
and pass no `-ngl`, so llama.cpp auto-fits. That is correct for a model that fits
and silently wrong for one that does not. On the 16 GB card, dense
`Qwen3.6-27B` and `gemma-4-31B` at Q8_0 do not fit; llama.cpp placed them on CPU
and served at 1.72 and 1.25 tok/s, in a directory asserting otherwise. The only
record is one log line, `layer 0 is assigned to device CPU`.

Cost: no accuracy cost. The same GGUF produces the same output wherever its
tensors sit, which the E4B llama.cpp/transformers control established. Every
latency and throughput comparison across that ladder is confounded, and one
conclusion was drawn from it: "MoE's gain is throughput" compared a
partly-resident MoE against a non-resident dense model. Corrected in
docs/LOCAL_INFERENCE.md to a fitting claim, which is what the numbers support.

Not fixed: the sweeps still do not record which device served each model. A
directory name is not provenance. Until they do, treat any speed number from
these ladders as device-unknown unless its server log has been read.

## Defect 16: the harness token cap scored as a model failure

See the Tier-B commit for the full case. `run_b.py --max-tokens` defaulted to
1024 while production allows `CURATOR_SYNTH_OUTBUF` (16384). With thinking left
on, models spend 400-1000 tokens reasoning before a short answer, so
`gemma-4-12B` truncated on one topic and scored zero on it. That single row moved
its format rate 1.0 -> 0.833 and coverage 1.0 -> 0.75, and I reported the result
as a model finding because it fit a pattern I already believed.

Cost: one wrong reported finding. `truncated: true` was in the prediction file
the whole time; nothing read it.

Fixed: cap is 4096, and `score_b.py` refuses to score a file containing a
truncated row rather than zeroing it.

## Defect 17: the scorer kept applying a gate the product had removed

`score.py --pred-key` defaulted to `pred`, the MF_CONF_FLOOR view. The floor was
removed from `src/kb/kb_memory_facts.c` and replaced by `fact_grounded()`, partly
*because of* this benchmark's evidence, and the default never moved. So every
Tier-A figure reported after that change was scored against a gate the shipping
code does not have.

The mechanism was already written down, in `run_hf.py`'s own docstring: the
prompt's schema example carries the literal `"confidence":0.0`, and small models
copy it verbatim. The floor was measuring prompt-copying, not extraction.
`src/kb/kb_memory_facts.c:48` says it outright: "Qwen3-0.6B commits nothing at
0.6, while 40% of what it extracts is correct."

Cost, by model, floored view -> shipping gate:

| model | floored | shipping | delta |
| --- | ---: | ---: | ---: |
| Qwen3-0.6B | 0.0000 | 0.4058 | +0.4058 |
| granite-4.0-350m | 0.0000 | 0.2063 | +0.2063 |
| Qwen3-1.7B | 0.4000 | 0.5937 | +0.1937 |
| granite-4.0-h-350m | 0.0000 | 0.1364 | +0.1364 |
| granite-4.1-3b | 0.5714 | 0.6522 | +0.0808 |
| LFM2.5-230M | 0.0000 | 0.0263 | +0.0263 |
| gemma-4-E2B | 0.6462 | 0.5793 | -0.0669 |
| gemma-4-E4B | 0.8281 | 0.8217 | -0.0064 |

The gate is not uniformly generous: it costs E2B 0.067, because grounding drops
extractions the floor let through. It is not a leniency change, it is a
correctness one.

Four of the six models I reported as scoring exactly 0.0000 are not zero under
the gate that ships. The claim built on that table, that nothing below about 600M
parameters produces usable extraction, was a claim about a retired config value.
Only SmolLM2-360M and gemma-3-270m are genuinely 0, and their failures are
specific and separately diagnosed:

| model | what it actually emits | genuine? |
| --- | --- | --- |
| gemma-3-270m | `{"content": "<the note, echoed back>"}` | yes, no schema at all |
| SmolLM2-360M | a bare fact object, no `facts` wrapper | yes, shape |
| LFM2.5-230M | `facts` array with `relation` and `object`, no `subject` | one missing field |
| granite-4.0-h-350m | correct extraction, JSON unterminated by one brace | truncated framing |
| LFM2-350M-Extract | correct shape, repeats one fact to the token cap | repetition loop |
| Qwen3-0.6B | correct schema on 97% of notes, 72 triples | no, config artifact |

Fixed: the default is now `pred_grounded`, synthesised in the scorer so every
prediction file already on disk gets the corrected view without re-running. `pred`
and `pred_nofloor` remain behind explicit flags, because the historical sweeps
were scored with the floor and the log above refers to those numbers.

Also fixed: `test_score.py` asserted the floored view was the default, so the
suite would have passed forever with the wrong gate. It now asserts the default
keeps a grounded low-confidence fact and drops an ungrounded high-confidence one.

Not fixed: the four non-zero small models were all measured with
`disable_thinking` set, which cost E4B 0.09. Their thinking-on numbers do not
exist yet.

## Defect 18: the refusal check was written over a cause, not an outcome

Defect 16 added a check that refuses to score a row which hit `--max-tokens`,
with the commit message "this class of defect fails loudly or it recurs". It
recurred within the hour. `Qwen3.6-27B` timed out on three of six Tier-B topics
against `--timeout 1800` and scored 0.50 format / 0.40 coverage, because the
check looked at `truncated` and not at `error`.

The check was written over one cause. It is now written over the outcome: a row
that produced no usable response is not evidence about the model unless the model
is what produced the emptiness. Truncation and transport error both block.

`--timeout` is also now settable from the sweep, because a per-request bound is a
harness choice like any other.

## Defect 19: the two lanes contended, and the file said they would

`sweep_b.sh` grew a `cpufit` lane so a model too large for the card could run
beside the GPU ladder instead of blocking it. The header claimed "it is CPU-bound
and the gpu lane is not, so serialising them buys nothing."

Three lines above, the same file describes serving an MoE with
`-ot ".ffn_.*_exps.=CPU"`, which is a CPU workload by construction.
`docs/LOCAL_INFERENCE.md` describes it too. Running dense `Qwen3.6-27B` on 8
threads beside `gemma-4-26B-A4B` drove load average to 39.6 on 20 cores:

| | uncontended | contended |
| --- | ---: | ---: |
| gemma-4-26B-A4B generation | 27.32 tok/s | 3.03 tok/s |
| Qwen3.6-27B per topic | ~9 min (est) | 28 min, 3 of 6 timed out |

Nine times slower, not the 10-20% the working assumption tolerates. Both results
discarded and both re-queued to run alone.

The correctness guard held: `prune_models.sh` refuses to delete weights a live
`llama-server` holds open, so the lanes never corrupted each other. The cost was
throughput and two wasted runs.

## Defect 20: overwriting a script while bash is executing it

`sweep_b.sh` was pushed to the CT twice while a sweep was mid-loop. Bash reads a
script incrementally by byte offset, so rewriting the file shifts everything
after the read point. The `cpufit` run completed its work and then died on
`line 150: unexpected EOF while looking for matching '"'` reading a stale offset
into the new file.

No result was lost, because a `for` loop is parsed as one compound command before
it runs, and the trailing `echo` is all that was left. That is luck, not design.
Push to a new path and swap, or stop the sweep first.

## Defect 21: run_llamacpp.py depended on torch it never used

`run_llamacpp.py` imports `CONF_FLOOR` and `extract_json` from `run_hf.py`,
which imported `torch` and `transformers` at module scope. So the llama.cpp
runner carried a hard dependency on a ~2GB GPU stack it never calls.

It never surfaced on .253, which has torch installed for the transformers lane.
On .254 the challenger control downloaded 10GB of weights, served them, and then
died on `ModuleNotFoundError: No module named 'torch'`.

Fixed: both imports moved inside `main()`, which is the only code path that uses
them. `run_llamacpp.py` now imports clean with no torch, and `test_score.py`
passes on .254.

## The cross-host control does not reproduce, and that is a finding

`gemma-4-12B`, same gold set, thinking on, same llama.cpp commit (0005475):

| | Q8_0 / 5080 / CUDA | Q6_K / 7900 XTX / Vulkan | delta |
| --- | ---: | ---: | ---: |
| strict F1 | 0.8235 | 0.8550 | +0.0315 |
| precision | 0.8116 | 0.8750 | +0.0634 |
| recall | 0.8358 | 0.8358 | +0.0000 |
| abstention | 0.8261 | 0.9130 | +0.0869 |
| predicted triples | 69 | 64 | -5 |
| schema / fabrication | 1.00 / 0.000 | 1.00 / 0.000 | unchanged |

Recall is IDENTICAL. Every gold fact the Q8 run found, the Q6 run also found.
The whole difference is five fewer predicted triples: the second configuration
abstains more on the empty-gold notes, so precision rises and F1 with it.

+0.0315 is larger than the entire measured gap between gemma-4-12B and
gemma-4-E4B (0.0018). So a challenger on .254 beating Gemma by 0.02 would be
inside this correction, and .254 numbers CANNOT be compared against the .253
ladder directly. They can only be read against a .254 control.

The delta bundles two changes, quantisation and backend. CUDA and Vulkan differ
in floating-point reduction order, so greedy decoding legitimately diverges
between them; this is not evidence that Q6 is "better" than Q8. A Q8 run on the
7900 XTX is queued to separate the two: Q8-here versus Q6-here is the quant
effect, Q8-here versus Q8-on-.253 is the backend effect.

Note also the latency column, which is not comparable at all: 781ms median here
against 11650ms on .253. The .253 thinking-ladder timings were taken while the
cpufit lane was saturating the box (defect 19), so every latency figure from
that ladder is contended and should not be quoted.

## Defect 22: two sweeps ran at once against one port, and nothing noticed

The .254 challenger sweep was launched, killed with `pkill`, and relaunched.
The `pkill` did not take, so two copies ran concurrently, both serving
GLM-4.7-Flash on port 8091. The second instance's startup also ran
`rm -f results/challenger-254/*.pred.jsonl` across the first one's output.

Nothing in the harness detected any of it. It was found by eye in a process
listing, while looking at something else.

Cost: none, as it turned out. The Q6 control was re-run under the lock and
scored bit-identical to the first run — 0.8550 / 0.8750 / 0.8358, 64 triples,
`strict` dict equal. That is luck. The control had completed before the second
instance started, and a request landing on the wrong server would not have been
visible in any output.

Fixed: `flock` on a single-instance lock, plus a pre-flight refusal if anything
is already answering on the port, whoever owns it. Both fail closed with a
message rather than proceeding.

The general lesson is the one already written down for container deploys and
ignored here: issue a stop, then ASSERT the stop took, in the script, before
acting on the assumption. `pkill` followed by a relaunch is not a stop.

## The cross-configuration noise floor is about 0.03 F1

`gemma-4-12B`, one gold set, one prompt, one llama.cpp commit, thinking on:

| config | F1 | precision | recall | triples | abstention |
| --- | ---: | ---: | ---: | ---: | ---: |
| Q8_0 / 5080 / CUDA | 0.8235 | 0.8116 | 0.8358 | 69 | 0.8261 |
| Q8_0 / 7900 XTX / Vulkan | 0.8438 | 0.8852 | 0.8060 | 61 | 0.9130 |
| Q6_K / 7900 XTX / Vulkan | 0.8550 | 0.8750 | 0.8358 | 64 | 0.9130 |

Split of the +0.0315 seen earlier:

- **quantisation**, Q6 against Q8 on the same card and backend: **+0.0112**
- **backend**, Vulkan against CUDA at the same quantisation: **+0.0203**

Neither is a capability change. Recall is not even monotonic across the three
(0.8358, 0.8060, 0.8358), and the abstention column tracks the F1 column almost
exactly: the configurations differ in how readily the model declines to emit on
the 23 empty-gold notes, not in what it can extract. On 69 notes one triple is
worth about 0.007 F1, so the whole 0.03 spread is roughly four triples.

Consequences worth holding onto:

1. **Q6 is not "better than Q8".** It is +0.011 on one model on one corpus, in
   the direction that abstention alone explains. Anyone reading these tables
   should not conclude a lower quantisation improves extraction.
2. **0.03 is the floor below which cross-host comparison says nothing.** A
   challenger scoring within 0.03 of Gemma 4 on a different host has not been
   shown to differ from it at all.
3. The 0.002 gap between gemma-4-12B and gemma-4-E4B, reported earlier as
   "within noise", is now quantified: it is a seventh of the noise floor.

## Defect 23: the Tier-A token cap manufactured the whole shape of the ladder

`sweep_thinking.sh` capped completions at 2048. Production allows
`MF_LLM_OUT_CAP`, which is 8192. Models that reason at length blew the cap and
emitted **nothing**:

| model | truncated | empty output |
| --- | ---: | ---: |
| gemma-4-E2B | 0 | 0 |
| gemma-4-E4B | 0 | 0 |
| gemma-4-12B | 8 | 8 |
| gemma-4-26B-A4B | 11 | 11 |
| gemma-4-31B | 0 | 0 |

The cap bites exactly the models that think longest, which on this ladder means
the larger ones. The empty rows were then counted **twice**: as abstentions,
inflating the abstention rate, and as missed facts, deflating recall. For
26B-A4B that read as abstention 0.78 -> 0.96 and recall 0.94 -> 0.84, and I
reported it as "thinking hurts the bigger model" with a mechanism invented to
fit it.

The 11 truncated notes for 26B-A4B were `ng01`-`ng05` (every negation note),
`im04`, `im07`, `am01`, `am05`, `gv05`, `mf03`. Negation is one of the categories
that actually separates models, so the cap removed the evidence from the place
it mattered most.

Two reported findings are retracted:

1. **"12B buys 0.002 over E4B, so the curve is flat from 4.5B to 12B."** 12B was
   scored on 62 usable notes against E4B's 70. The comparison was never valid.
2. **"Thinking hurts gemma-4-26B-A4B."** No evidence for it. The thinking-on run
   never produced output on 11 notes.

This is the same defect as 16 and 18, in its third and fourth location.
`score_b.py` learned it for `--max-tokens` and again for `--timeout`; `score.py`
had no such check at all. It now refuses any run containing a truncated row.

The bitter part: `sweep_thinking.sh`'s own header said "the thinking pass
consumed the completion budget before the JSON, committing zero facts. If that
recurs here it should show as truncation, not as a mystery." I predicted the
failure, instrumented for it, set the constant that causes it, and then read the
resulting numbers as model behaviour.

Note for anyone re-reading the ladder: gemma-4-31B truncated 0 times even at
2048, so it was not contaminated by this. It was discarded and re-run anyway,
because half a ladder under one cap and half under another is not a ladder.

## Defect 24: the challenger sweep ran reasoning models with reasoning off

`sweep_challenger_254.sh` invoked `run_llamacpp.py` without `--thinking` and
without `--max-tokens`, so every model on .254 ran with reasoning suppressed
against the runner's **default 512-token cap** — a sixteenth of production's
`MF_LLM_OUT_CAP`. The .253 ladder it was meant to be compared against runs
`--thinking --max-tokens 8192`.

| model | truncated | median completion tokens |
| --- | ---: | ---: |
| gemma-4-12B Q6 (control) | 0/70 | 34 |
| gemma-4-12B Q8 (control) | 0/70 | 33 |
| Magistral-Small-2509 | 0/70 | 30 |
| **Olmo-3.1-32B-Think** | **59/70** | 512 |
| GLM-4.7-Flash | 70/70 | 512 |

The sweep existed to test reasoning-tuned models, and it ran them with reasoning
off. Olmo-3.1-32B-**Think** truncated on 59 of 70 notes. Magistral scored 0.7376
and I reported it as "works, clearly worse" — with its reasoning suppressed.

The controls did not truncate, because 12B without thinking emits ~34 tokens, so
nothing in the control's own numbers hinted at the problem. That is what made it
survive: the check I had just added fires on truncation, and the two runs I was
using to validate the host were the two that could not truncate.

Withdrawn as a result:

- **Magistral-Small-2509 at 0.7376.** Measured in the wrong configuration.
- **The quantisation/backend split (+0.0112 / +0.0203).** The .254 side was
  thinking-off at 512, the .253 side thinking-on at 2048. Four variables, not
  two, and the .253 half has since been discarded under defect 23 anyway.
- **The ~0.03 cross-host noise floor.** Rested on the above.

Not withdrawn: **GLM-4.7-Flash emits garbage.** Probed outside the harness
entirely, on the raw `/completion` endpoint with no chat template:

```
prompt:  "The capital of France is"
output:  '????????????'
```

That is not a cap, a prompt or a flag. Q6_K GGUF under RADV Vulkan on the 7900
XTX produces non-language. Whether the cause is the quantisation, the GGUF
publisher or the Vulkan backend is untested; the discriminating run is
GLM-4.7-Flash on .253 under CUDA with expert offload.

Fixed: the sweep now passes `--thinking --max-tokens 8192` explicitly, with a
comment saying they are not optional. All .254 results deleted and re-running.

The deeper problem is that `run_llamacpp.py`'s defaults (512, thinking off) are
not production's, so any caller that forgets a flag silently measures a
different system. Two sweeps have now done exactly that.

## Defect 25: the runner's defaults were not production's, so omission was silent

`run_llamacpp.py` defaulted `--max-tokens` to 512 (production allows 8192) and
treated thinking as a bare `store_true`, so "off" was indistinguishable from
"not considered". Any sweep that forgot a flag quietly measured a different
system than the one that ships, and two did.

An audit of every sweep that calls the runner, after the fact:

| sweep | thinking | cap |
| --- | --- | --- |
| sweep_thinking.sh | `--thinking` | 8192 |
| sweep_challenger_254.sh | MISSING, now fixed | 512, now fixed |
| **sweep_sub1b.sh** | **MISSING** | **512 default** |
| sweep_llamacpp.sh | MISSING | 512 default |
| sweep_dense_31b.sh | MISSING | 512 default |
| sweep_dense_vs_moe.sh | MISSING | 512 default |
| sweep_noconf.sh | MISSING | 512 default |
| sweep_q4_accuracy.sh | MISSING | 512 default |

`sweep_sub1b.sh` had not run yet. It was queued to re-measure exactly the models
whose earlier numbers were an artefact, and it would have reproduced the artefact
in a new form.

Fixed three ways:

1. `--max-tokens` defaults to **8192**, matching `MF_LLM_OUT_CAP`. A caller who
   forgets it now measures the shipped system.
2. Thinking is a **required mutually-exclusive group**: `--thinking` or
   `--no-thinking`, no default. It is worth +0.09 F1 to gemma-4-E4B, the largest
   effect measured here, so a run that does not record which side it took is not
   interpretable. Omitting it is now an argparse error rather than a silent
   choice.
3. Every historical sweep marked `--no-thinking` explicitly, so the lanes in
   `results/` stay reproducible now that the default has changed meaning.

The general form: a benchmark's defaults should be the product's defaults.
Where they differ, every difference has to be stated at each call site, and
nothing enforces that. Where they cannot match, the parameter should have no
default at all.

## Defect 26: score.py refused truncation but not transport errors

The fourth appearance of one defect, and the second time only half a lesson was
carried across. `score_b.py` refuses both truncated and errored rows, learned in
two separate incidents. `score.py` was given the truncation check and not the
error check.

`Magistral-Small-2509` then recorded `RemoteDisconnected` on the first note and
`Connection refused` on the next 68, because a process kill of mine took its
llama-server down mid-run. Without the check that scores as a model emitting
nothing on every note.

Fixed: `score.py` refuses any run containing an errored row, naming the first
three.

## Defect 27: pgrep -f matches the command doing the pgrep

Three times now, a cleanup command of the form
`pgrep -f <pattern> | xargs kill` has killed its own SSH session, because the
pattern appears in the command line of the shell running it. Once it also failed
to stop the target: `pgrep -f sweep_challenger_254 | head -1` returned my own
wrapper first, killed that, and left the real sweep running — which is how two
sweeps ended up serving the same port a second time.

Cost: one wasted .254 sweep, two interrupted sessions, and the invalid Magistral
run above.

The fix is not another kill loop. It is that a sweep should be stoppable by its
own lockfile rather than by pattern-matching a process table.

## What .254 is for

Recorded because it was not clear at the start and the confusion cost real work.

`.254` is a **triage** host: does a candidate load, hold the output contract, and
produce anything worth spending `.253` GPU time on. It is deliberately the place
to try a model nobody expects to work, without queueing it behind the real
ladder.

It is **not** a measurement host. Different quantisation (Q6_K/Q5_K_M, forced by
24GB VRAM against 3GB of system RAM), different backend (RADV Vulkan against
CUDA), and it runs other workloads.

The consequence, which cost an evening: no cross-host control is needed, because
no cross-host comparison should be made. Anything promising on .254 is re-run on
.253 for a number. The Q6/Q8 calibration study built to enable that comparison
was work in service of a comparison that should never have been attempted.

## Defect 28: the sweeps reported OK for runs that produced nothing

`run_llamacpp.py` and `run_b.py` exit 0 even when every row carries a transport
error, because they record failures per note rather than aborting. The sweeps
tested only that exit status, so a run whose server had been killed mid-note
still printed `OK`.

Observed directly: `GLM-4.7-Flash.q6` was killed mid-run, the scorer correctly
refused it, `cp` failed loudly on stderr for a score file that did not exist, and
the very next line was:

```
cp: cannot stat 'results/challenger-254/GLM-4.7-Flash.q6.score.pred_grounded.json': No such file or directory
OK   GLM-4.7-Flash.q6
```

Every safeguard fired and the sweep announced success anyway. A sweep whose
success signal is wrong is worse than one that fails, because the summary line
is the only thing anyone reads at a glance.

Fixed in every sweep still in use: OK is printed only when the score file exists
and is non-empty, otherwise FAIL with the scorer's own message, and the
prediction file is removed so the next pass re-runs it.

Not fixed: roughly a dozen historical sweeps under `harness/sweep_*.sh` have the
same shape. They have already run and their outputs are indexed in
`evidence/RUNS.md`, which audits the artefacts rather than trusting the summary
line, so the index is the reliable record either way.

## GLM-4.7-Flash does not work on this hardware

Triage result, and it is a hard no on .254 rather than a quality judgement:

- Raw `/completion`, no chat template, no harness: `"The capital of France is"`
  returns `'????????????'`.
- Through the chat template with thinking on: 7943 completion tokens, all of
  them `reasoning_content`, zero content, on every note. Not truncated; the cap
  is 8192.
- Generation runs at **0.68 tok/s** for a 30B-A3B MoE resident on a 24GB card,
  roughly a fiftieth of what the hardware should give. At ~8000 tokens per note
  that is over three hours per note and the run cannot complete.

Together those say the Q6_K GGUF under RADV Vulkan is not executing this
architecture correctly, not that the model is bad at extraction. The
discriminating run is GLM-4.7-Flash on .253 under CUDA with expert offload,
which is queued. Until that lands, nothing at all is known about GLM's quality on
this task.

## Defect 29: the self-healing pass deleted a live run out from under it

`verify_and_heal.sh` asks the scorer whether each prediction file is scoreable
and deletes the ones it refuses, so the next sweep pass re-runs them. An
**in-progress** run is incomplete by definition, so the scorer refuses it, so
the heal loop deleted it — while the runner was still writing.

The runner keeps its file descriptor. It went on writing to an unlinked inode,
and every row would have vanished the moment it exited:

```
$ ls -l /proc/<pid>/fd
... -> /opt/tierA/.../GLM-4.7-Flash.pred.jsonl (deleted)
```

Neither the sweep nor the runner reported anything. The only symptom was a
prediction file that never appeared while a healthy server logged 11 tok/s.

Cause: two chains both call `verify_and_heal` on `results/thinking`, and the
orphans chain's call landed while phase 3's GLM run was 22 minutes into a
~2.5-hour corpus. The heal pass was written as if it only ever ran between
sweeps.

Recovered rather than re-run: the fd was still open, so the data was reachable
through `/proc/<pid>/fd` and a copier loop pulled it out while the run
continued.

Fixed: `verify_and_heal` refuses to touch any file a live process holds open,
found by scanning `/proc/*/fd` (lsof is not installed on the bench containers).
The check is on the file rather than on a process name or a lock, because the
file is the thing that must not be deleted.

Verified both directions on the host, not assumed: with a holder alive the file
is reported `LIVE` and survives; with the holder gone the same file is healed
and removed.

The general shape, and it is the counterpart to every other defect in this log:
a safety mechanism with no notion of concurrency is itself a hazard. Every
earlier defect here was a check that failed to fire. This was a check that fired
when it should not have, and it was the most destructive of them.

## Defect 30: the `.254` lane measured an integrated GPU for a week

Every `challenger-254` run was taken on an **8GB Phoenix iGPU sharing a 14GB
host**, not the 24GB 7900 XTX the lane notes claim. The card is physically
present at `0000:6b:00.0` (Navi 31, `1002:744c`) and had **no driver bound**, so
`llama-server --list-devices` enumerated exactly one device — the iGPU — and
llama.cpp took it by default. A reboot on 2026-08-01 bound `amdgpu` and both
devices appeared:

    Vulkan0: AMD Radeon Graphics (RADV PHOENIX)   8170 MiB   <- what we were using
    Vulkan1: AMD Radeon RX 7900 XTX (RADV NAVI31) 24560 MiB  <- what we thought

### What this invalidates

Two conclusions in this log are wrong and are retracted here rather than edited
in place, because the reasoning is instructive:

**"GLM-4.7-Flash does not work on this hardware."** Recorded on three symptoms:
`????????` from a raw completion, 7943 reasoning tokens with no content, and
0.68 tok/s for a 30B-A3B MoE "resident on a 24GB card". It was never resident on
any card — a ~19GB Q6 model against 8GB of shared memory on a swapping host
explains all three without any architecture bug. GLM subsequently ran clean on
`.253` under CUDA at 10.7 tok/s and scored **F1 0.7801**, the best recall in the
benchmark. The Vulkan/RADV theory was invented to explain a fit problem.

**Magistral-Small-2509 "too slow to finish".** 19 minutes per note in `D` state
is what thrashing looks like, not what the model costs.

`gemma-4-12B-it.q6` at 0.8630 stays in the index because accuracy plausibly
survives — the same GGUF answers the same wherever its tensors sit — but its
speed and fit numbers are void.

### The general lesson

Every sweep records a `device.json`, which was supposed to prevent exactly this.
It recorded the *requested* placement (`-ngl 99`) and the *absence of CPU-offload
warnings*, and both were satisfied by an iGPU. Provenance that cannot distinguish
two devices is not provenance.

`sweep_quant.sh` now pins `--device` explicitly and records `device_used`
alongside `device_requested`, because a host with more than one GPU will silently
give you the wrong one and every number will look plausible.

## Defect 31: one sentence in the prompt turned E4B's reasoning off

gemma-4-E4B emitted **zero** reasoning tokens on every note of the v4 10k run.
Not truncated, not disabled by a flag: the prompt asked for it to stop.

The trigger is the closing sentence, `No prose, no markdown.` E4B applies it to
its own thought channel as well as to its answer. Bisected on the shipped prompt,
20 notes, greedy, thinking requested:

| system prompt | thinks | median reasoning |
|---|---:|---:|
| v4, unmodified | 0/20 | 0 |
| v4 minus `No prose, no markdown.` | 20/20 | 1222 |
| v4 minus `Return ONLY a JSON object:` | 0/20 | 0 |

Everything upstream was fine and was checked rather than assumed: E2B and E4B
ship byte-identical chat templates (same SHA, same five `enable_thinking`
branches), and `run_llamacpp.py` was sending `chat_template_kwargs.enable_thinking`
correctly all along. No config bug, no bad quant, no broken template.

### Why it survived a week of benchmarking

Nothing fails. The output is valid bare JSON, parses clean, and scores 0.5947.
There is no error, no warning, and no truncation — the only symptom is a number
that is lower than it should be, which is indistinguishable from the model simply
being worse. It is also scale-dependent: **E2B does not have the behaviour**, so
the two arms of the same sweep disagreed for a reason that looked like model
size. Thinking is worth +0.084 F1 to E4B on this set, the single largest effect
measured here, and the prompt was discarding it silently.

### The fix is not deletion

Deleting the sentence restores thinking and costs the guardrail it was there for:
**14 of 20** answers come back fenced in ```json. The production parser scans
first-`{` to last-`}` (`kb_memory_facts.c:283`) so nothing breaks downstream,
which is precisely why the regression would not have been noticed either.

Rescoping the constraint to the answer does **not** work. `The answer itself must
be a JSON object only, with no prose or markdown around it` still gives **0/20**:
the model is not drawing a distinction between its answer and its reasoning, so
the exemption has to be granted outright rather than implied.

v5 ships the grant, measured on both builds:

| variant | UD-Q4_K_XL thinks / fenced | ggml-org Q8_0 thinks / fenced |
|---|---:|---:|
| v4 control | 0/20 / 0 | 0/20 / 0 |
| answer-only rescope | 0/20 / 0 | 0/20 / 0 |
| drop the sentence | 20/20 / 16 | 20/20 / 14 |
| **v5 `Reason first if it helps;`** | **20/20 / 2** | **20/20 / 0** |

### It is the model, not Unsloth's repack

Worth stating because it was the obvious objection and it was tested rather than
argued: every E4B number in this benchmark had been taken on an Unsloth Dynamic
quant, so "the UD quant broke thinking" was a live alternative. It is wrong twice
over. The UD build thinks freely under the other variants, so nothing was baked
out of it; and the stock `ggml-org/gemma-4-E4B-it-Q8_0` conversion — different
quantisation **and** a different chat template (sha `603a42db…`, 18566B, against
unsloth's `74a88f94…`, 18807B) — reproduces the suppression exactly, 0/20.

The template difference is its own caution. E2B and E4B matched byte-for-byte
*within* Unsloth, which is what made "same template" feel safe; across vendors the
same model does not ship the same template. Comparing templates only inside one
vendor's repo answers a narrower question than it appears to.

Reproduce: `harness/probe_thinking_prompt.py`, which now also measures whatever
tail the shipped prompt currently carries, so the live prompt is re-tested rather
than trusted.

### What this invalidates

Every E4B result in this log was taken with reasoning off while the run recorded
`thinking: true`, because the flag was sent and honoured — the prompt overrode it
downstream of the flag. The **v4 10k E4B figure of 0.5947 is a thinking-off
number** and is not comparable to any E2B figure, which was thinking-on. E4B's
arms need re-running under v5 before any model-to-model claim in this benchmark
holds. E2B's numbers are unaffected.

## The benchmark was fast because it was broken

Worth separating from defect 31, because it is the part that would have made
anyone suspicious if they had been looking at the right number.

The v4 10k E4B arm finished in about 34 minutes. That was taken as a fact about
the hardware. It was a fact about the prompt:

| | v4 10k E4B, as banked | same card, thinking on |
|---|---:|---:|
| median completion tokens | **27** | ~390 |
| median latency | **214 ms** | ~1790 ms |
| notes that emitted reasoning | **0 / 10000** | 20 / 20 |
| throughput | 280 notes/min | 27 notes/min |

Same 5080, same CUDA build, same quant, same corpus. The only difference is
whether the model was allowed to think.

**The token count is not the tell, and reading it as one is a mistake.** 27
tokens is the correct size for this answer: `{"facts":[]}` is 5 tokens and one
triple is about 30, so the distribution (p10 5, median 27, p90 49, 3475 notes
returning no facts at a median of 5) is exactly what a healthy extractor emits.
`parse_ok` was 10000/10000 and nothing was truncated. The answer channel was
never unhealthy — only the reasoning channel was, and the answer channel is the
one every tool here looks at.

The throughput gap is a consequence of the defect, not evidence that would have
found it.

The practical consequence is that the honest cost of the v5 pair is 12-16 hours
rather than the ~1 hour the v4 timings implied. The old figure was never
achievable with reasoning on; it was the cost of a benchmark that had turned the
model's reasoning off.

There is a general form of this worth keeping: a performance number that improves
for no reason you designed is evidence about correctness, not about performance.

### The evidence that WAS there, and why nothing fired

Every one of the 10000 rows carried these two fields side by side:

```json
{"thinking": true, "reasoning_chars": 0, "parse_ok": true, "truncated": false}
```

A row that asserts thinking was requested and reasoning was empty is
self-contradictory, and the contradiction was written ten thousand times without
anything objecting. `reasoning_chars` was added during the GLM triage — where
the question was whether 7943 reasoning tokens contained anything real — and then
never read again. `score.py` does not mention the field, and neither does
`summarize.py`. It was write-only telemetry.

So the honest answer to "how was this scored at all" is that the scorer scores
triples against gold and has no notion of how they were produced. A run with
reasoning off is, to it, a slightly worse run. Recording a signal is not the same
as checking it, and a field that nothing consumes will not save you no matter how
diligently it is written.

`score.py` now refuses a run whose rows claim `thinking:true` while no row
carries any reasoning, on the same footing as its existing refusal of truncated
runs.

## v5 on E2B: no measurable cost, one inert side effect

v5 exists for E4B. E2B never suppressed its reasoning, so for E2B the change is
all downside-risk and no upside, and shipping it on the assumption that a longer
prompt is harmless would have been exactly the kind of untested step this log is
full of. 401 notes sampled proportionally by category from the v4 mid corpus,
both arms against the same server process, same notes, same seed:

| arm | F1 | precision | recall | thought | fenced | median tokens |
|---|---:|---:|---:|---:|---:|---:|
| v4 | 0.5831 | 0.5797 | 0.5865 | 401/401 | 0 | 467 |
| v5 | 0.5599 | 0.5511 | 0.5689 | 401/401 | 98 | 478 |

The F1 gap is **not resolvable at this n** and is not claimed: paired bootstrap
over 5000 replicates gives -0.0232 with a 95% CI of [-0.0515, +0.0057], which
crosses zero. The point estimate is negative and the interval permits a real cost
of up to five points, so this establishes "no measured harm", not "no harm".
Resolving a 0.02 effect needs roughly four times the notes.

E2B thought on 401/401 notes under BOTH prompts, which is the direct confirmation
that defect 31 is E4B-only rather than something about the corpus or the harness.

The one unambiguous change is fencing: 0 fenced answers under v4, 98 of 401 under
v5. That is a deterministic behavioural difference, not noise, and it is inert —
those 98 rows are 98/98 parse_ok and 98/98 schema_ok, and their facts-to-gold
ratio (125/118) matches the unfenced rows (227/223). The production parser scans
first-`{` to last-`}`, so a fence changes nothing downstream. Noted rather than
fixed, because the alternative wordings that fence less on E2B are the ones that
fail to restore thinking on E4B, and E4B's +0.084 is a measured effect while this
is a cosmetic one.

## Defect 32: the +0.084 that justified everything was an n=70 result

The v5 prompt change, the provider_client fix, and the decision to re-run the
whole E4B ladder all rest on one number: "thinking is worth +0.084 F1 to
gemma-4-E4B". It is quoted in `kb_curator_provider.c`, in `provider_client.c`,
and in the v5 commit message.

Its provenance is `results/thinking/gemma-4-E4B-it.score.json`: F1 0.8217 from
**53 true positives against 67 gold triples**, roughly 70 notes. The comparison
run scored 0.738. Nobody put an interval on either.

The v5 10k run was stopped at 955 notes, which is enough to check it. Same model,
same quant, same card, same corpus, paired on the same notes:

| | strict F1 | precision | recall | tp | fp | fn |
|---|---:|---:|---:|---:|---:|---:|
| v4, thinking suppressed | 0.5990 | 0.6607 | 0.5478 | 481 | 247 | 397 |
| v5, thinking restored | 0.6093 | 0.6175 | 0.6014 | 528 | 327 | 350 |

**+0.0103, 95% CI [-0.0201, +0.0404], INDISTINGUISHABLE** over 5000 paired
bootstrap replicates on 878 gold triples — thirteen times the evidence behind the
original claim.

The two are not strictly the same measurement: the old sweep used the 70-note
gold set, which is much easier (F1 ~0.82 against ~0.60 here), and the effect may
genuinely differ by corpus. But +0.084 cannot be quoted as a measured constant in
production comments on that basis, and it has been.

### The effect is real, and strict F1 is the wrong instrument for it

Stopping there would repeat the mistake in the other direction. The error audit
says v5's 93 extra false positives are mostly not errors:

| | FP | predicate_variant | partial_overlap | symmetric | genuinely spurious | FN |
|---|---:|---:|---:|---:|---:|---:|
| v5 | 312 | 130 | 116 | 11 | 54 | 331 |
| v4 | 219 | 89 | 99 | 1 | 30 | 394 |

About 68 of the 93 are reconcilable by `rel_type_canonicalize()` and the entity
graph — the mechanisms production already runs — and 24 are genuinely spurious.
Meanwhile v5 recovers 63 gold facts v4 missed.

Scored on entity pairs, ignoring predicate naming:

| | relation-agnostic F1 | precision | recall |
|---|---:|---:|---:|
| v4 | 0.7783 | 0.8585 | 0.7118 |
| v5 | **0.8390** | 0.8503 | **0.8280** |

Recall +0.116 at flat precision. Thinking finds materially more real facts; it
also names them more variably, and strict scoring charges that variance twice —
once as a false positive and once as a false negative. No CI on this one: the
bootstrap tool scores strict only, so +0.061 is a point estimate.

Fabrication is 0.0 on both arms, 0 ungrounded triples out of 855 predicted, so
the extra volume is not invention.

The cost is abstention: 0.9067 -> 0.8700 on empty-gold notes, 28 -> 40 spurious
triples. A model that reasons is less willing to say nothing, which is the same
effect the negation slice shows.

### What to take from it

Two claims were being made at once and only one survives. "Thinking is worth
+0.084 strict F1" does not reproduce. "Thinking materially improves extraction"
does, at +0.116 recall on entity pairs, and it was invisible in the headline
metric because that metric punishes predicate variance the product reconciles
anyway. The honest summary is that thinking trades naming discipline and
abstention for coverage, and whether that is worth it depends on which of those
the KB values — which is a product question, not a benchmark one.

## v6 at 1k: retractions reach the storage layer, and polarity does not leak

First full run of the polarity prompt. gemma-4-E4B UD-Q4, 1001 notes of the v4
small corpus, thinking on, 5080/CUDA.

Scored in two halves, because the gold cannot judge the half it was written
against: every retraction note is labelled EMPTY under the v1-v5 policy, so
grading v6 against it naively charges a false positive for each retraction v6
correctly emits. That number would be real and meaningless.

**Ordinary extraction, negated facts removed, gold untouched:**

| view | F1 | precision | recall |
|---|---:|---:|---:|
| strict | 0.5858 | 0.5983 | 0.5739 |
| relation-agnostic | 0.8144 | 0.8318 | 0.7977 |

In the same band as v5, so the polarity field did not disturb normal extraction.
NOT a clean v5-vs-v6 comparison: this is gold_small and the v5 figure came from
the first 955 of gold_large, which are different notes of different difficulty.
A real comparison needs v5 re-run on this corpus.

**Retraction quality, 132 negation notes:**

| | count |
|---|---:|
| flagged with negated=true | 114 / 132 |
| directly usable by `db2_fact_retract` | **91** |
| invented / non-canonical relation | 23 |
| empty object | 2 |

"Usable" means a canonical relation AND a non-empty object, because `target`
scopes the retraction and an empty one blanks every value of
(source, relation). The 2 empty-object cases never reach the API: the malformed
check in mf_commit_facts rejects them first, verified by line order rather than
assumed.

The 23 invented relations are `withdrew_from`, `partnered_with`,
`did_not_renew` — and they are largely harmless for a reason worth noting.
"Ashcombe Networks pulled out before signing" retracts a fact that was never
asserted, so `db2_fact_retract` matches no edge and returns 0. A retraction of
something that does not exist is a no-op, which is the right failure mode for a
mechanism that deactivates data.

**Polarity leak, the failure mode that would sink the design:**

75 of 869 non-retraction notes emit a negated fact, and on inspection that is
mostly correct rather than leakage:

| | count |
|---|---:|
| contradicts a currently-true gold fact | **1** |
| retracts some other value (an old path on a move) | 74 |
| notes that ALSO emitted the correct replacement fact | 64 / 75 (85%) |

One error in 869 notes. The rest are moves — "lib.rs is no longer in X but in
Y" — where negating the old location is exactly right, and 85% of the time the
model emits both halves: retract the old value, assert the new one. That is the
change-of-state behaviour the KB has never been able to record.

The single genuine error is instructive: "airflow-install.sh in ProxmoxVED is
now called apache-airflow-install.sh" produced
`(airflow-install.sh, also_known_as, apache-airflow-install.sh, negated)` —
negating the rename the note asserts. A rename reads like a replacement, and
`also_known_as` is symmetric, so there is no old value to retract.

### The end-to-end path, now verified

The 1k run said models can produce the field. It did not say the path works,
and that gap was the important one: `db2_fact_retract` has been complete and
tested since P3 while nothing called it, which is precisely what testing an API
and a prompt separately hides.

`test_memory_facts_retract.c` drives `mf_commit_facts` on a raw model response
— through JSON parsing, the grounding gate and relation canonicalisation — and
asserts against the stored graph rather than a return value:

- a negated fact deactivates the edge and commits nothing (a retraction
  legitimately reports 0 assertions; the effect is in the graph)
- it hits only the NAMED edge: two live `member_of` values, one retracted, the
  other survives. This is the assertion that justifies putting polarity on the
  original fact — an empty `target` means every current value of
  (source, relation), so dropping `object` would take both
- a move commits the new value and retracts the old without cancelling out
- absent or false polarity commits exactly as before
- an UNGROUNDED retraction is refused, because the grounding gate runs before
  the polarity branch. Retraction is destructive and "delete an edge for
  someone the note never mentions" is the worst thing this path could do
- an EMPTY object cannot blank a whole relation

Verified red before green: with `if (negated)` forced false the first case
fails. The full unit-tests suite passes with it added.

What remains unverified is the deployed path — no live KB has run a retraction
end to end, only the sqlite shim — and the 1 polarity error per 869 notes is
measured on one model and one corpus.

## v5 vs v7 on one corpus: the first unconfounded prompt comparison

Every earlier prompt comparison here was confounded by the corpus. v5's numbers
came from a slice of gold_large and v6's from gold_small — different notes of
different difficulty — so the gap between them said as much about the corpus as
about the prompt, and the honest thing was to refuse to compare them. Both arms
were re-run on the same 1001 notes, same server process, same quant, same
session, one variable.

**A correction to the first attempt.** Scoring v7 with its negated facts stripped
(the gold has no notion of them) flatters it: on a retraction note v5's spurious
positive counts as a false positive while v7's retraction is simply removed. That
is the "let the policy decide the answer" error inverted. Excluding retraction
notes from BOTH arms is the apples-to-apples version, and the result survives it:

| view | v5 | v7 | delta |
|---|---:|---:|---:|
| strict F1 | 0.5856 | **0.6102** | **+0.0246** |
| lenient F1 | 0.6158 | 0.6381 | +0.0223 |
| relation-agnostic F1 | 0.8393 | 0.8202 | **-0.0191** |

869 non-retraction notes, paired bootstrap over 5000 replicates:
**+0.0246, 95% CI [+0.0050, +0.0452], significant.** The first prompt delta in
this whole effort with an interval that excludes zero.

Abstention improves 0.8758 -> 0.8913 and spurious triples fall 42 -> 35.
Fabrication is 0.0 on both.

### The two views disagree, and that is the finding

Strict rises while relation-agnostic falls. Those measure different things:
strict charges predicate naming variance twice, relation-agnostic ignores naming
and asks only whether the right entity pair was found. So v7 names predicates
more canonically and finds slightly fewer pairs — it trades a little coverage for
naming discipline and better abstention.

That is a plausible consequence of the change: v6/v7 tell the model to use "the
same canonical relation the positive fact would use" and name two predicates
explicitly, which is naming guidance whether or not the note is a retraction.

Two limits, stated rather than smoothed: the relation-agnostic delta has NO
interval — bootstrap_ci.py scores strict only — so -0.0191 is a point estimate.
And v5 -> v7 bundles the polarity change with the rename sentence, so nothing
here attributes the gain to either alone.

## Mining two runs for dataset and system defects

Two full runs of the same 1001 notes under different prompts (v5, v7) give a
test the single-run analyses could not: when two DIFFERENT prompts independently
produce the same triple the gold rejects, the gold is the likely defect. That is
how defect 7 was found, with models standing in for prompts. 191 such triples.

### Defect 33: the corpus phrases a hostname fact as "runs on"

28 of the 51 `has_hostname` gold triples come from notes worded "X runs on Y".
The remaining 23 say "has hostname". Split by phrasing, the model's behaviour is
not ambiguous:

| note phrasing | n | model emits has_hostname | model emits runs_on |
|---|---:|---:|---:|
| "X has hostname Y" | 23 | **23/23** | 0 |
| "X runs on Y" | 28 | **0/28** | 23 |

Identical in both runs. The model is perfect when the note says hostname and
scores zero when it says runs on, because "runs on" is a deployment relation and
"has hostname" is a naming one. They are different facts and the corpus
conflates them — the generator derives hostnames from service names and then
phrases some of them as deployment.

Cost: 28 false negatives and 23 false positives per run from one template, about
3% of the gold, and it penalises exactly the models that read the sentence
correctly.

Fix is in the corpus, not the prompt: phrase the note the way the labelled
relation reads, or label these as a deployment relation. Not applied here
because regenerating the corpus invalidates every run taken against it.

### Defect 34: the object-kind gate cannot fire on the LLM path

`mf_commit_facts` sets `obj_kind = NODE_OTHER` unconditionally — the extractor
supplies no kinds — and then coerces it to the relation's declared type whenever
OTHER is not allowed:

    if (!rel_type_kind_allowed(sdef, 0, obj_kind) && sdef->tail_kind_count > 0)
       obj_kind = sdef->tail_kinds[0];

**14 of the 17 seed relations do not allow NODE_OTHER as a tail**, so for those
the object kind is overwritten on every single extraction and
FACT_GATE_REJECT_KIND can never fire on an object. The gate is structurally dead
on this path.

It was added for a good reason — a wrong kind guess silently dropped facts — but
the effect is that a mistyped object is not rejected, it is relabelled. "Oakhaven
Publishing member_of enterprise" stamps `enterprise` as an ORG; "Rosa Ostrowski
has_role redgrave contract" stamps a contract as a SCALAR. The entity registry
accumulates wrong kinds, and those kinds gate future writes.

The distinction the code is missing is between "this object is of kind OTHER" and
"nobody told me the kind". Both are NODE_OTHER today. An explicit unknown would
let the gate defer rather than coerce.

### Defect 35: the ontology does not cover the domain, and auto-promotion papers over it

19% of the GOLD's own triples (167 of 880, across 12 predicates) use relations
the seed ontology does not define — `owns_account` (39), `subscription_tier`
(39), `customer_of` (26), `purchased` (17), `runs` (8), `audits` (6). So the
benchmark requires the model to invent a predicate and then grades it on guessing
the same invented word. The gold is not even self-consistent about it: both
`owns` and `owns_account` appear.

The model's side matches: 22-24% of extracted facts use a non-seed predicate,
89 distinct novel predicates across the two runs, 54 of them seen exactly once.

These facts are NOT stranded — a NOVEL verdict still writes the edge, and
`db2_fact_recall_block` filters on `superseded_at`/`suppressed`, not on class or
rel_type status, so they are recallable. The cost is fragmentation. The same
relationship arrives under several names:

| family | facts | split across |
|---|---:|---|
| hosting/deployment | 112 | runs_on 45, has_hostname 46, operates 16, hosts 5 |
| ownership | 89 | owns 59, acquired 30 |
| membership | 396 | works_for 205, member_of 167, contributes_to 24 |

Auto-promotion (threshold 3) then makes this permanent: **23 of the 89 novel
predicates recur often enough to be promoted to active**, which would grow the
active ontology from 17 to ~40 relations, most of them near-synonyms of each
other. A recall for "which host does X run on" has to know four spellings.

The fix is ontology coverage, not more aliases after the fact: the recurring,
semantically distinct relations this domain needs (customer relationship,
subscription tier, account ownership, deployment) should be seeded, so the model
lands on them instead of inventing a word and the promoter admitting it.

## Polarity holds on a second model

The retraction design was adopted on E4B alone, which is one model's habits. E2B
is the right second opinion: it never had the thinking defect and behaves
differently on retractions. Both arms, v7, 1001 notes of gold_small, same server,
scored against the 17-relation ontology they actually ran under (the ontology
grew to 24 afterwards, so judging them by the new list would credit relations
the model was never shown).

| | flagged / 132 | usable by db2_fact_retract | polarity errors / 869 |
|---|---:|---:|---:|
| E4B | 115 | 92 | **0** |
| E2B | 85 | **85** | **1** |

"Usable" is a canonical relation AND a non-empty object — what the API can
consume without a human deciding what was meant.

The two models fail differently and both fail safely. E4B flags more retractions
and spends 23 of them on invented predicates; E2B flags fewer and **every single
one it flags is directly usable**. Higher recall with lower naming precision
against lower recall with perfect naming precision — worth knowing before
choosing a synthesis model for this task, and not visible in an F1 column.

One polarity error across 1738 non-retraction notes on two models. The failure
mode that would have sunk the design — polarity leaking onto ordinary facts —
did not appear at a rate that matters.

Both numbers should improve under the 24-relation ontology, since runs_on,
owns_account and customer_of are now canonical and were among the invented
predicates counted against E4B here. That is a prediction, not a measurement:
these runs predate the change.

## The 10k tier, and what it took to make one comparable

Before 2026-08-03 only two models had ever been run on `gold_large`: E2B and E4B.
Everything else in the project topped out at 1001 notes or at the 70-note
`data/gold.jsonl`, so article 1's ranking compared a 1001-note granite against a
1001-note E2B taken by a different sweep at a different setting.

All six models now have a 10,000-note arm on `data/corpora/v5/gold_large.jsonl`,
nproc=3, cache-ram 1024, UD-Q4_K_XL, prompt v8, thinking requested:

| model | strict F1 | precision | recall | parse_ok | wall | MTP |
|---|---:|---:|---:|---:|---:|---|
| gemma-4-E4B | 0.6301 | 0.5874 | 0.6796 | — | 159m | yes |
| gemma-4-E2B | 0.6246 | — | — | — | 146m | yes |
| granite-4.1-3b | 0.5627 | 0.5622 | 0.5632 | 9974 | 21m | none published |
| gemma-3n-E4B | 0.5424 | 0.4992 | 0.5938 | 9989 | 47m | none published |
| Qwen3-1.7B | 0.4591 | 0.4500 | 0.4685 | 9895 | 361m | none published |
| granite-4.0-1b | 0.4215 | 0.4118 | 0.4317 | 9563 | 16m | none published |

Every arm: 10,000 rows, zero transport errors, zero truncation, fabrication rate
0.0 where computed.

**The ranking is not yet clean, and the reason is in the last column.** The two
gemma-4 arms ran with MTP drafts because they came from the ladder; the other
four cannot, because no `mtp-*.gguf` is published for them. MTP moves 26 of 100
notes (finding 12), so the draft head currently sits inside the comparison. The
no-MTP ladder now running is what removes it.

### Three arms scored only under protest

`granite-4.0-1b`, `granite-4.1-3b` and `gemma-3n-E4B` recorded `thinking: true`
and produced zero reasoning characters on all 10,000 rows, so `score.py` refused
them under the defect-31 guard and they were scored with `--allow-thinking-off`.
Qwen3-1.7B and every gemma-4 arm scored with no flags.

Recorded as an observation, not a diagnosis: whether these three have no thought
channel, or have one that this prompt closes, is not established here. The
distinction matters and is cheap to settle from `/props`.

### 22:1 on wall clock, at similar quality

granite-4.0-1b finished 10,000 notes in 16 minutes; Qwen3-1.7B took 361 on the
same card at the same settings. Median completion tokens is the mechanism -- 33
against Qwen3's much longer reasoning-bearing outputs -- and the two land 0.04 F1
apart. No interval was computed on that gap.

### Abstention separates the two granites more than anything else does

granite-4.0-1b abstains on 31.5% of factless notes; granite-4.1-3b on 75.7%.
Both were scored identically. That is the larger part of the 0.14 F1 between
them, and it is a behavioural difference rather than a capability one.

## Process count, measured a second time

finding 19 put 1-vs-3 processes at 0.0105 F1. gemma-3n-E4B at 10k, same card,
same everything else:

| processes | strict F1 | precision | recall | wall | rows/min |
|---|---:|---:|---:|---:|---:|
| 4 | 0.5429 | 0.5000 | 0.5939 | 47m | 217 |
| 3 | 0.5424 | 0.4992 | 0.5938 | 47m | 212 |
| delta | **-0.0005** | | | | |

Whether 3-vs-4 is small because the step is smaller than 1-vs-3, or because the
effect is model-dependent, is not established by one pairing and no interval was
computed. It cost nothing in throughput either way.

The arm was originally taken at nproc=4 because its driver passed `NPROC=0` and
let the sizer choose. That choice was made from the model's 5.02 GiB **file
size**; resident VRAM is 3414 MiB. Sizing a run from file size is the defect --
the sizer was right and the assumption feeding it was wrong.

## The cache-ram re-run closed, and moved almost nothing

The three E4B 10k arms were re-taken at `--cache-ram 1024` to match the E2B
ladder, and the 8192 originals quarantined rather than deleted:

| arm | at 8192 | at 1024 | delta |
|---|---:|---:|---:|
| E4B.UD-Q4_K_XL | 0.6324 | 0.6301 | -0.0023 |
| E4B.UD-Q6_K_XL | 0.6450 | 0.6452 | +0.0002 |
| E4B.UD-Q8_K_XL | 0.6321 | 0.6337 | +0.0016 |

Single pairings, no intervals. The re-run was not done because the old numbers
looked wrong -- it was done because cache-ram is results-affecting and the two
families have to share the value before they can be compared. That reasoning
stands whatever the deltas turned out to be, and they turned out to be small.

The full ladder now shares one cache-ram value:

|  | Q4 | Q6 | Q8 |
|---|---:|---:|---:|
| E2B | 0.6246 | 0.6344 | 0.6329 |
| E4B | 0.6301 | 0.6452 | 0.6337 |

### One inconsistency, recorded and unexplained

Reasoning coverage is not uniform across the ladder: 10000/10000 on all three E2B
arms, 9989 on E4B Q4, 9994 on E4B Q8, and **8673 on E4B Q6**. All are above the
guard's zero threshold so all scored without flags. No explanation is offered
here because none has been measured.

## Defect 35: the throughput metric included server startup, and startup scales with nproc

`notes/min` in the scaling sweeps is rows divided by wall clock, and wall clock
starts before the servers do. Measured startup, median of both modes:

| card | nproc=1 | nproc=2 | nproc=3 | nproc=4 |
|---|---:|---:|---:|---:|
| 5080 | 56s | 84s | 107s | 137s |
| XTX | 61s | 67s | 83s | 99s |

Roughly 30s of model load per additional server. On a 200-note run that is a
third to a half of the wall clock, and **it is not a constant across the
configurations being compared** -- it grows with the variable under test. So the
metric systematically penalised higher process counts, which is what the sweep
existed to measure.

Corrected by computing steady-state throughput from per-request latency and
process count (`nproc * 60 / median_latency_s`) instead:

| card | nproc | MTP | no-MTP | ratio |
|---|---:|---:|---:|---:|
| 5080 | 1 | 47.6 | 30.1 | 1.58x |
| 5080 | 2 | 67.4 | 36.7 | 1.84x |
| 5080 | 3 | 59.9 | 34.4 | 1.74x |
| 5080 | 4 | 61.8 | 33.6 | 1.84x |
| XTX | 1 | 40.7 | 21.7 | 1.87x |
| XTX | 2 | 63.8 | 34.7 | 1.84x |
| XTX | 3 | 78.1 | 41.2 | 1.89x |
| XTX | 4 | 83.3 | 43.6 | 1.91x |

### Two claims retracted

Both were made from the contaminated metric and both were wrong.

**"Aggregate throughput peaks at nproc=2 and declines."** It plateaus. Steady
state on the 5080 is 47.6 / 67.4 / 59.9 / 61.8 -- flat after two processes, not
falling.

**"nproc=4 is 25% below peak and slower than a single process."** nproc=4 is
FASTER than nproc=1 on both cards (61.8 vs 47.6 on the 5080, 83.3 vs 40.7 on the
XTX). The apparent decline was entirely the extra 80s of startup that four
servers cost over one.

### What survives, and is stronger for the correction

**MTP is worth 1.58x-1.91x, and it does not depend much on process count.** Eight
paired measurements across two backends land in that band. finding 12's 1.59x
sits at the bottom of it, measured at nproc=1 where this table also reads 1.58x.

**The scaling curves differ by backend.** The 5080 (CUDA) flattens after two
processes; the XTX (RADV Vulkan) is still climbing at four, 40.7 -> 83.3. The
project runs nproc=3 on both cards, chosen by what fits in VRAM. On the XTX that
leaves throughput on the table; on the 5080 it is past the point where more
processes buy anything.

Per-stream throughput falls with every added process on both cards (5080 MTP
359 -> 108, XTX MTP 294 -> 151) while aggregate rises or holds. Adding processes
does not create throughput; it divides existing throughput into more, slower
streams, and stops helping once the card is saturated.

## The 5.3x MTP speedup was an artefact, and is withdrawn

Reported earlier from the 10k arms: E2B Q4 on the XTX at nproc=3 ran 68.5
notes/min with MTP against ~13 without, so MTP looked worth 5.3x.

It was a comparison between two things that were never comparable. The 68.5 is a
**completed 10,000-note average**, startup amortised over 146 minutes. The ~13
came from a run **sampled while it was still early**. Dividing them produced a
number that is a property of neither.

The paired sweeps put the real figure at 1.89x for that exact configuration.

### The residual, which is NOT explained

Startup correction reconciles the MTP side: the banked 10k arm's 68.5 sits
against the sweep's steady-state 78.1, a 12% gap that longer-run effects can
plausibly cover.

It does not reconcile the other side. The 10k no-MTP arm ran at ~13 notes/min
where the sweep's steady state for the identical configuration is 41.2 -- a 3x
gap that startup cannot touch, because the 10k arm amortises startup away.

The signature, recorded while that arm was live: the server reported 53ms prompt
eval and 4638ms decode per request, total 4.7s, while the client measured 13.7s.
Nine seconds per request outside the server's own accounting. The 200-note sweep
shows no such gap -- its client latency matches its token counts.

Candidates, neither tested: prompt-cache pressure at 10,000 distinct notes
against 200 (finding 20's territory, and `--cache-ram 1024` holds roughly 38
entries), or a transient on that specific run. The discriminating test is the
same configuration on ~2000 notes, watching whether the rate decays with corpus
position. It has not been run.

**Until it is, no 10k throughput number should be compared against a 200-note
one in either direction.**

## Defect 36: killing a sweep leaves its clients running, and the next run on the same ports silently competes with them

`shard_run.sh` launches one `run_llamacpp.py` per shard and backgrounds them.
Nothing reaps those children if the wrapper dies. Kill the wrapper -- or the
driver above it -- and three python clients survive with PPID 1, still issuing
requests to `BASE_PORT..BASE_PORT+n` forever.

This is a correctness defect, not a tidiness one. The next arm launched on the
same `BASE_PORT` shares its servers with the orphans. Every request is served
normally, it just waits, so **the server's own timings look perfectly healthy**
and only the client sees the cost.

Measured 2026-08-04 after a day of stopped and relaunched lanes:

| | run_llamacpp.py processes | E2B Q4 no-MTP, nproc=3 |
|---|---:|---:|
| 15 orphans + 3 live | 18 | **8.8 notes/min** |
| orphans killed | 3 | **37-39 notes/min** |
| 200-note sweep, ports nothing else used | 3 | 41.2 notes/min |

Killing fifteen orphaned clients took the same in-flight arm from 8.8 to 38.7
notes/min immediately, with no other change.

### What this invalidates

**The 5.3x MTP speedup, already withdrawn, was this.** The no-MTP arm ran while
orphans held the same ports; the banked MTP arm had run before any existed.
Dividing them measured process contention, not speculation.

**The "9 seconds per request unaccounted for by the server".** Server-side cost
was 4.7s while the client measured 13.7s. That gap was queueing behind orphaned
requests -- invisible to per-task server timing by construction.

**The "3x unexplained residual" between 10k arms and the 200-note sweeps.** The
sweeps ran on ports 8900 and 8950. Every 10k ladder attempt ran on 8700. Only the
ladder had orphans on its ports, which is why the two disagreed and why the
disagreement looked like a corpus-size effect. It was not: prompt-cache pressure
was the leading hypothesis and it was wrong.

**Not invalidated:** the scaling sweeps themselves. They used unused ports and
their internal comparisons were clean, which is now confirmed rather than assumed
-- their 41.2 figure matches the 37-39 measured once the ports were clear.

### Fixed

`shard_run.sh` now traps EXIT/INT/TERM and kills its recorded child PIDs.

The general form, and the third variant of it in this log: a background process
whose lifetime is not tied to its parent's. defect 27 was `pgrep -f` matching the
process doing the matching; defect 29 was a healing pass deleting a live run.
This is the same family -- process management assumed rather than enforced -- and
it cost most of a day plus one fabricated headline number.

### The tell that was available the whole time

`uptime` read a load average of 27 on a workstation whose only job was to shuttle
JSON over three ssh tunnels. Nothing in the harness looks at load, and no
diagnostic printed the client count. `pgrep -c run_llamacpp` would have answered
it in one second at any point in the preceding six hours.

## Defect 37: a model that answers in a tool-call envelope scores ~0 and looks incapable

LFM2.5-230M returned strict F1 **0.0022** on 1001 notes. It is not failing at
extraction. It is answering in a different envelope:

```
<|tool_call_start|>[{"name": "facts", "arguments": {"subject": "Vera Duarte",
  "relation": "joined", "object": "retrieval team", "confidence": 0.9,
  "negated": false}}]<|tool_call_end|>
```

That is the correct triple for the first note -- the same one gemma-4 produces.
The harness expects `{"facts":[...]}`, so the row parses as JSON (parse_ok
1000/1001) but fails the schema (schema_ok 16/1001), and 1001 notes yield 22
predicted triples.

**982 of 1001 rows** used the tool-call envelope.

Re-parsing those rows and re-scoring:

| | strict F1 | precision | recall | tp | fp | fn |
|---|---:|---:|---:|---:|---:|---:|
| as scored by score.py | 0.0022 | - | - | - | - | - |
| re-parsed from tool-calls | **0.1564** | 0.1517 | 0.1614 | 142 | 794 | 738 |

The harness understates this model by roughly **71x**.

### Both numbers are wrong to publish alone

0.0022 measures the envelope, not the model. 0.1564 is not harness-validated --
it comes from an ad-hoc re-parse rather than score.py's audited path, and the
mapping from tool-call arguments to the fact schema was chosen by hand.

What is safe to say: even parsed generously, 230M lands at ~0.156 against a field
whose next-lowest member is granite-4.0-1b at 0.3911. It is genuinely weak at
this task AND genuinely mis-measured. Either fact alone misleads.

### Scope, unmeasured

Unknown how many other arms in this benchmark are affected. `results/sub1b/`
holds seven models scored at 70 notes, including an earlier LFM2.5-230M at
0.1061, and nothing has checked whether their low scores are capability or
envelope. The check is cheap -- grep the `raw` field for `tool_call` -- and has
not been run across the corpus of banked predictions.

### Not fixed

score.py was NOT changed. Adding tool-call parsing mid-benchmark would alter the
scoring path for every banked arm and break comparability with everything already
measured. The decision of whether to add it, and re-score the field, belongs to
whoever owns the article -- it is a change to the instrument, not a bug fix.

### Defect 37, continued: the quant decides the envelope

LFM2.5-230M at Q6_K scored 0.1363 where Q4_K_M scored 0.0022. Same model, same
prompt, same harness, same 1001 notes. The difference is the output format, and
it is a switch rather than a gradient:

| quant | tool-call envelope | parse_ok | schema_ok | triples | strict F1 |
|---|---:|---:|---:|---:|---:|
| Q4_K_M | **982/1001** | 1000 | 16 | 22 | 0.0022 |
| Q6_K | **0/1001** | 1001 | 1001 | 925 | 0.1363 |

Quantisation changed which envelope the model emits. Nothing in the pipeline
noticed, because `parse_ok` is 1000/1001 in BOTH cases -- the tool-call payload is
valid JSON, it is just the wrong shape. Only `schema_ok` moves, and no driver
looks at it.

This also resolves the open question from the first half of this defect. The
ad-hoc re-parse of the Q4 arm gave 0.1564; Q6 gives 0.1363 through score.py's
normal audited path. Two independent routes agree that the model's real
capability is ~0.14-0.16 -- last in the field by a wide margin, and roughly 60x
above what the Q4 arm appeared to score.

The publishable claim is therefore stronger and simpler than "a model used the
wrong envelope": **a quantisation choice silently changed a model's output format
and moved its apparent score by 60x.** Anyone running a quant ladder on a model
they have not inspected can hit this, and the symptom is a number that looks like
the model being bad at the task.

Cheap detector, not currently in any driver: count rows whose `raw` contains
`tool_call`, or simply alert when `schema_ok` diverges from `parse_ok`.

## Defect 38: the truncation flag is unreachable by construction

`run_llamacpp.py` sets `"truncated": usage.completion_tokens == args.max_tokens`.
Every driver passes `--max-tokens 8192`, and `shard_run.sh` starts every server
with `-c 8192`. Completion is therefore capped at `8192 - prompt_tokens`, roughly
7630 with this benchmark's ~560-token prompt. **`completion_tokens` can never
equal `max_tokens`, so the flag can never be true.**

Found on MiniCPM5-1B Q4_K_M, where 292 of 1001 rows returned EMPTY content after
exhausting the context on reasoning:

| | value |
|---|---:|
| rows | 1001 |
| parse_ok | 661 |
| failed to parse | 340 |
| empty raw content | **292** |
| rows flagged truncated | **0** |
| median completion tokens, rows that parsed | 1980 |
| median completion tokens, empty rows | **7632** |
| prompt + completion on every empty row | **8192 exactly** |

All 292 sit exactly at the context limit. None was flagged.

### Scope: every arm in this project

The detector has never been able to fire, so no banked result has ever been
checked for truncation. Sampling committed arms for rows at the context limit:

| arm | rows at context limit | flagged truncated |
|---|---:|---:|
| MiniCPM5-1B Q4_K_M | 292/1001 | 0 |
| Qwen3-1.7B 10k | **54/10000** | 0 |
| LFM2.5-2.6B Q4_K_M | 1/1001 | 0 |
| gemma-4-E2B Q4 10k | 0/10000 | 0 |

The gemma arms never approach the limit so nothing is at stake there, which is
why this survived. Qwen3-1.7B's 10k arm -- already committed and used in the
field ranking -- contains 54 silently empty rows.

### Why this is defect 16/18's third appearance

Defect 16 added a refusal for rows that hit `--max-tokens`, with the commit
message "this class of defect fails loudly or it recurs". Defect 18 recorded that
it recurred within the hour because the check was written over a cause rather
than an outcome. This is the same failure a third time: the check is written over
`max_tokens` (a cause) instead of "the row produced no usable content" (the
outcome), and a configuration where the two limits coincide makes it silent.

### The correct check, not applied

`prompt_tokens + completion_tokens >= context_size` catches it, as does the
outcome-shaped test the earlier defects already argued for: refuse a row whose
content is empty. score.py is NOT changed here -- adding a refusal now would
invalidate banked arms mid-campaign, and that is the article owner's call.

MiniCPM5-1B Q4_K_M's F1 of 0.1258 is a floor, not a capability measurement: 29%
of its corpus produced nothing and was scored as a miss.

## Defect 38, full scope: published scores that are context exhaustion, not capability

The truncation flag has never been able to fire (see above), so no banked arm was
ever checked. Auditing every arm in `results/sub1b/` and `results/thinking/` for
empty output, parse failure and rows sitting at the context limit:

| arm | published F1 | n | parse_ok | schema_ok | empty | at context limit |
|---|---:|---:|---:|---:|---:|---:|
| **Qwen3.5-0.8B** | **0.0000** | 70 | 0 | 0 | **70** | **70** |
| **gemma-3-270m-it** | **0.0000** | 70 | 0 | 0 | 0 | **70** |
| **Qwen3.5-2B** | **0.3210** | 70 | 9 | 9 | **61** | **61** |
| **LFM2-350M-Extract** | **0.0144** | 70 | 14 | 14 | 0 | **56** |
| Qwen3.6-35B-A3B | — | 70 | 6 | 6 | **64** | 0 |
| gemma-4-31B-it | — | 70 | 58 | 58 | **12** | 0 |
| granite-4.0-h-350m | 0.2045 | 70 | 22 | 22 | 0 | 0 |
| GLM-4.7-Flash | 0.7801 | 70 | 64 | 64 | 6 | 6 |
| gemma-4-12B-it | 0.8472 | 70 | 67 | 64 | 3 | 3 |
| SmolLM2-360M-Instruct | 0.0000 | 70 | **70** | **0** | 0 | 0 |

**Two models scored 0.0000 and it does not mean what it appears to mean.**
Qwen3.5-0.8B produced no content on all 70 notes, every one at
`prompt + completion == 8192` with a median 7933 completion tokens of reasoning.
gemma-3-270m-it likewise sat on the limit for all 70. Neither number measures
extraction ability; both measure a model reasoning past the context window.

**Qwen3.5-2B's 0.3210 was computed on 9 of 70 notes.** The other 61 were empty.

**SmolLM2-360M-Instruct is a different failure again**: it parses cleanly on all
70 rows at a median 308 tokens -- nowhere near the limit -- but matches the schema
on zero. That is the envelope problem of defect 37 without the tool-call frame.
Its 0.0000 is also not capability.

**It reaches the large models too.** Qwen3.6-35B-A3B has 64 of 70 rows empty and
gemma-4-31B has 12. Those are two of the models whose figures are quoted in this
log's dense-vs-MoE comparison.

### What this means for anything already published

The sub1b table and the 70-note model table both contain numbers that are floors
or artefacts rather than measurements, and nothing in the pipeline distinguished
them because the only detector was unreachable. Any article quoting a 0.0000, or
ranking models near the bottom of those tables, is reporting harness behaviour as
model behaviour.

The rows that are clean -- gemma-4-E4B 70/70, granite-4.1-3b 70/70, gemma-3n-E4B
70/70 -- are clean. The problem is that nothing said which was which.

### Not fixed

score.py is unchanged. Adding a refusal now would invalidate arms mid-campaign
and is the article owner's call. The detector needed is one line, and this table
is what it would have printed at any point in the past year:

    prompt_tokens + completion_tokens >= context_size    # exhausted
    schema_ok < parse_ok                                 # wrong envelope

## Defect 39: 0.0105 was used as a significance threshold all campaign, and it is not one

Every "inside noise" / "3.4x the threshold" judgement in this campaign compared a
delta against **0.0105**. That number is finding 19's measured effect of changing
process count from 1 to 3. It is a specific configuration difference on a
specific model. It is not a confidence interval, and it was never derived from
the sampling distribution of anything.

Caught when the user questioned an apparent E4B < E2B result. Running the paired
bootstrap this project already has (`harness/bootstrap_ci.py`, 5000 replicates,
same notes resampled for both arms):

| comparison | delta | 95% CI | verdict |
|---|---:|---|---|
| E4B - E2B, QAT q4_0 | -0.0213 | [-0.0437, +0.0016] | indistinguishable |
| E4B - E2B, UD-Q4_K_XL | +0.0150 | [-0.0085, +0.0390] | indistinguishable |

**The real interval at n=1001 is roughly +/- 0.024**, more than double the 0.0105
that has been standing in for it.

### What this changes

Both directions of the E2B/E4B comparison are noise. Across all five
configurations where the two were run head to head, the sign flips and no gap
clears its interval. Given E2B is architecturally a nested submodel of E4B, the
supportable statement is that **the submodel matches its parent on this task to
within what 1001 notes can resolve** -- which is a stronger and more interesting
claim than either ordering, and the one that should be written.

### What survives

Deltas comfortably outside +/- 0.024 are unaffected:

  SmolLM3-3B  Q8 vs Q4      +0.0352
  gemma-4-E2B QAT vs UD     +0.0389

Deltas that were called null and stay null, being far inside it:

  MTP vs no-MTP, E2B Q4/Q6/Q8   +0.0039 / +0.0013 / -0.0022

### What needs re-checking

Anything this campaign called significant on the strength of the 0.0105 figure
alone, particularly the LFM2.5-2.6B quant spread (-0.0104, described as "exactly
at the noise threshold" -- it is well inside a +/- 0.024 interval and should be
reported as flat) and any model-to-model gap in the 1001-note ranking under
0.024, which includes several adjacent pairs.

The tool to do it correctly has existed in this harness the whole time and was
used twice in this campaign, both times for MTP. It should have been used for
every comparison.

## Defect 40: corpus composition changes per-note output, so a subset is not a run

Running gemma-4-E2B-it-qat on gold_small (1001 notes) and on gold_mid (3002
notes, which strictly contains gold_small), same card, same quant, same nproc=1,
same prompt, same everything:

| | value |
|---|---:|
| 1001-note subset of the 3002 arm, strict F1 | 0.6327 |
| independently-run 1001 arm, strict F1 | 0.6406 |
| difference | -0.0079 |
| **byte-identical completions on the shared 1001 notes** | **529 / 1001** |

**47% of outputs differ on identical inputs in an identical configuration.**

Per defect 39 the difference carries its own interval rather than a fixed
threshold. Paired bootstrap over the shared 1001 notes, 20000 replicates:

> extracted_from_3002 - native_1001 = **-0.0079, 95% CI [-0.0278, +0.0114]**
> -- INDISTINGUISHABLE

The error budget moves almost entirely into precision: fp goes 338 -> 356 while
fn is flat at 306 -> 308 and tp barely moves, 574 -> 572. So the churn is not
symmetric noise; the 3002-note context yields marginally more spurious spans.
n=1001 cannot resolve a difference this size, and confirming it would need the
gap re-measured at the mid or large tier rather than argued from these counts.

This contradicts the standing claim in `articles/README.md` that "within one
configuration, repetition is exact", which rests on three runs of the same
three-process arm agreeing byte-for-byte on all 1001 notes. That claim is true
for *re-running the same corpus*. It is false across corpora that share notes.

### The mechanism is cache history, and the obvious test does NOT isolate it

The natural hypothesis is the immediately preceding note: gold_small's notes are
scattered through gold_mid (positions 1 to 3000), and 675 of the 1001 have a
different predecessor in the mid run. Splitting the churn by that:

| | outputs differing |
|---|---:|
| same predecessor in both runs | 146/326 (44.8%) |
| different predecessor | 326/675 (48.3%) |

Nearly identical. The predecessor explains nothing.

The consistent reading is that `--cache-ram 1024` holds roughly 38 entries
(finding 20's arithmetic: ~26 KiB per token, ~213 MiB per full context), so the
state carried into any request is the last ~38 notes, not the last one. Between a
1001-note and a 3002-note corpus almost every note has a different 38-note
history, which predicts uniform churn -- and uniform churn is what appears. That
is consistent with the mechanism but is not a measurement of it; the
discriminating experiment (vary cache-ram, or run with the cache disabled) has
not been done.

### What this invalidates

**Subset extraction is not equivalent to running at that tier.** This campaign
built `results/subset-1001/` by extracting the 1001 gold_small notes from banked
10k arms and scoring them, then ranked new 1001-note arms against that table. The
extraction is internally consistent -- every model was subset the same way, so
model-to-model comparisons within that table hold -- but any comparison between a
**natively-run** 1001 arm and a **subset-extracted** one carries this effect.
That includes the LFM2.5 and newcomer models, all run natively at 1001, ranked
against a field extracted from 10k.

The F1 impact measured here is -0.0079, well inside the +/-0.024 interval that
n=1001 supports, so no ranking conclusion in this campaign changes. The byte-level
churn of 47% is much larger than the F1 movement, which is the same pattern MTP
shows: outputs move a great deal and the aggregate score barely notices.

### The explanation is WITHDRAWN. The prompt cache is not the mechanism.

A discriminating run was registered with its prediction stated in
`harness/cache_isolation_5080.sh` before it started: re-run both corpora with
`--cache-ram 0`, and if cache history is the mechanism the two cache-off arms
must agree on the shared 1001 notes at or near 1001/1001.

| | byte-identical on the shared 1001 |
|---|---:|
| cache ON (the original observation) | 529/1001 (52.8%) |
| **cache OFF** | **499/1001 (49.9%)** |

Disabling the cache did not reduce the churn. It is marginally worse. The
prediction fails, so the explanation is withdrawn rather than softened.

The flag semantics were checked rather than assumed, because the whole test rests
on them: `llama-server --help` documents `-cram, --cache-ram N` as
"set the maximum cache size in MiB (default: 8192, -1 - no limit, 0 - disable)".
0 disables.

**Both candidate mechanisms are now dead.** Predecessor identity was refuted at
44.8% against 48.3%. Cache history is refuted here. No third mechanism is
proposed, because this defect has already cost two explanations that fitted the
data and did not survive their own tests.

### The correction to the cost claim, which points the other way

This entry previously recorded the comparability run as costing "hours per arm",
reasoning from finding 20 that the ~600-token system prompt is served from the
cache and disabling it re-evaluates the prefix per note.

Measured: **38 minutes with the cache off against 41 with it on**, same 1001
notes, and 116 minutes for the 3002-note arm at an identical 25.7 notes/min.
Prefilling 600 tokens is noise next to two seconds of generation. The run that
was declined for two days as unaffordable was free, and the claim that it was
expensive was reasoning rather than measurement.

That also means the prompt cache buys nothing measurable in throughput at
nproc=1 on this workload, which is worth stating on its own.

### What the cache costs in accuracy: nothing measurable, at two tiers

| tier | cache ON | cache OFF | paired delta | 95% CI |
|---|---:|---:|---:|---|
| gold_small, n=1001 | 0.6406 | 0.6466 | +0.0059 | [-0.0074, +0.0194] |
| gold_mid, n=3002 | 0.6416 | 0.6396 | -0.0021 | [-0.0092, +0.0049] |

Opposite signs, both indistinguishable, and the tighter interval at n=3002 is the
better estimate. Turning the prompt cache off is free in both wall time and
accuracy, so there is no longer a tradeoff to weigh: **for any arm that will be
compared across corpora, run with `--cache-ram 0`.**

### SOLVED: sequence position, on a prediction registered before the run

Third hypothesis, stated in `harness/order_test_5080.sh` before it started: with
the prompt cache off and per-corpus determinism confirmed, the only thing still
differing between a gold_small run and a gold_mid run is WHERE each note sits in
the sequence. Test: the same 1001 notes, seeded shuffle, everything else held.

| | byte-identical |
|---|---:|
| same notes, same order, cache off | **1001/1001 (100%)** |
| same notes, SHUFFLED order, cache off | **524/1001 (52.3%)** |
| same notes, inside gold_mid, cache off | 499/1001 (49.9%) |

Shuffling reproduces the cross-corpus churn to within 25 notes. **Sequence
position is the mechanism.** A subset is not a run because its notes sit
somewhere else in the queue.

Note what this implies and what it does not. It implies state carries between
requests even with `--cache-ram 0`: llama-server keeps a live KV context per slot
across requests, and `--cache-ram` governs the prompt cache rather than that
context. It does NOT identify which state, and I am not going to name one without
another registered test.

**Three hypotheses, two dead, one confirmed.** Predecessor identity: refuted,
44.8% against 48.3%. Prompt-cache history: refuted, 49.9% against 52.8%. Sequence
position: confirmed. The two failures were both explanations that fitted the data
and did not survive their own tests, which is why the third was registered in
writing before the run rather than after seeing the number.

### The consequence for every tier in this project

`gold_small` is a strict subset of `gold_mid` is a strict subset of `gold_large`,
and that containment is now known NOT to give comparability. Scoring a 10k arm
down to 1001 notes measures those notes at their 10k positions, which is a
different configuration from running them as a 1001-note corpus.

The fix is cheap and known: for any arm intended for cross-corpus comparison, the
notes must occupy the same positions. Same tier, same order, no exceptions.

### Still narrower, and still open

Whether corpus composition is the variable at all. Every explanation so far has
assumed this configuration reproduces itself, and that was measured with the
cache ON at nproc=3 on the XTX (1001/1001, three ways). Cache OFF at nproc=1 on
the 5080 has never been checked. `harness/cacheoff_selfrepro_5080.sh` runs
gold_small a second time in the identical configuration; if it does not match
run 1 byte for byte, corpus composition was never the variable and this defect
has been measuring plain nondeterminism.

## Defect 41: MiniCPM5-1B does not serve under this harness, and I kept paying to rediscover that

`openbmb/MiniCPM5-1B-GGUF` has now failed on three separate rented hosts and
earlier on local hardware. The signature is identical every time: the container
starts, the port maps, and `/health` never answers, so the placement logic
concludes the host is bad and re-places onto another one.

**The re-placement logic was the wrong response.** It was written for a host
problem (a stalled image pull), and it correctly handles that. Applied to a model
that cannot load, it turns one failure into an unbounded sequence of paid
failures, each of which looks like a fresh host problem in the log. Three hosts
showing the same symptom is evidence about the model, not about the hosts, and
nothing in the pool was counting that.

**Not fixed by retrying, and no longer attempted.** MiniCPM5-1B is removed from
every job list. What is already known about it is banked and sufficient for the
head-to-head: at 1001 notes on local hardware it scores 0.1652 at Q8_0 and 0.1258
at Q4_K_M, with parse rates of 0.87 and 0.66. Both figures are floors caused by
parse failure rather than capability measurements, and they are reported that way.

**The rule this earns:** a failure that repeats across independent hosts is a
property of the thing being placed. Count failures per JOB, not per host, and
stop after the second.

## Defect 42: a finished arm's server keeps the card

`arm_26b_unsloth_5080.sh` started a server on port 8992 and never stopped it. When
the 3k pair launched on the same card, `shard_run.sh` cleared its own port range
(8300-8307), found it clean, and started a server that immediately failed with
`cudaMalloc failed: out of memory`: 6390 MiB requested, 14828 of 16303 MiB still
held by the finished 26B.

The launcher reported `sizing: starting one server to measure resident VRAM` and
sat there. Nothing in the local logs said OOM; the message was inside the
container, in a file named after the port. Cost was about 12 minutes.

**Symptom to recognise:** a card at high VRAM and 0% utilisation with no client
running. That is the same signature I used on the rented fleet to identify
abandoned instances, and it means the same thing locally.

**Rule:** whatever starts a server stops it, in the script that started it.
Clearing a port range proves nothing about a card.
