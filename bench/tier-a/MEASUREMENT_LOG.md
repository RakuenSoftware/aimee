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
search strategy (look for places where "different" was being scored as "wrong") and it kept paying out.

The ninth (incomplete gold) is worse and subtler: the metric punished answers
that were correct and *not labelled at all*, which penalises the models that
extract most. That one biases the ranking rather than just depressing it.

---

## 1. Symmetric relations scored as wrong

**Found by** hand-reading E4B's disagreements after its score looked low.

`rel_type_def_t` carries `is_symmetric`, and the C comment states "one assertion
implies both directions"; `knows` and `spouse` are symmetric, so `(sarah, spouse,
user)` is correct, but the scorer charged it twice, as a false positive *and* a
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

**Found by** validating a brand-new metric before trusting it, the one time
checking happened *before* publishing rather than after.

The grounding check flagged `kb_server` against "KB server", `7` against
"seven", and `me` as invented entities. **Qwen3.5-0.8B appeared to fabricate
11.4% of its triples; the true figure is 0.0%.**

**Lesson** a new metric deserves the same suspicion as a new model.

## 5. Schema validity: the one that inverted a conclusion

**Found by** an observation from outside: schema validity fell monotonically with
model size (1.00 → 0.96 → 0.84 → 0.77 across E4B, 12B, 26B, 35B), which is a
strange thing for bigger models to be worse at.

All 30 "schema failures" across all four models were the literal string `{}` or
`[]`, on notes asserting no durable fact. **Zero were malformed.** The models
were abstaining correctly and saying so tersely; the runner recorded anything
without a `facts` array as a failure. Production agreed with the models, not the
metric, `mf_commit_facts()` commits nothing either way.

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

## 7. Incomplete gold: the largest single error

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
renderings are equally correct, naming a device by description rather than
hostname, an `alt` attaches to the specific gold triple and scores as a true
positive, rather than being excused from the denominator.

## 8. Equivalent predicates

`speaks` vs `speaks_language`, `attends` vs `member_of`, `daughter` vs
`child_of`. Each cost a model both a false positive and a false negative for a
naming choice carrying no information.

Handled by an explicit equivalence table, kept deliberately narrow: `studied_at`
is **not** equivalent to `studied`, because "studied medicine" and "studied at
Otago" are different facts; Bare kinship terms were also missing from the
*production* alias table, so this one found a real product defect too.

## 9. Object containment

`"2 of the junior engineers"` for `"junior engineers"`, `"proxmox host in the
auckland rack"` for `"auckland rack"`, token-F1 0.5 and 0.571, just under the
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
  these were overwhelmingly *genuine model errors*, `lives_in` where the note
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
| `sync_results.sh` rescoring | the CT's stale scores silently reverting local fixes: this happened **three times** |
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
variation is normalisation's job; predicate variation is expected and fine,
`works_for` vs `member_of`, `born_in` vs `grew_up_in`. Renaming an endpoint is a
different node and a different claim.

**Impact** the first corrections in the exercise that moved DOWN. E4B 0.723 ->
0.692.

**A related error of framing.** I had defended endpoint leniency by noting that
aimee has entity aliasing, so `build host` and `forge` would resolve to one node
downstream. That is not a reason the extraction was correct. This benchmark
measures extraction, not aimee's pipeline, and that aliasing code exists to
compensate for an imperfect embedder, not to license loose scoring. **Strict is
the headline metric**: both endpoints exact, predicate flexible, no assumption of
a resolution step that is not being measured.

## 11. Per-model error audits: predicate coverage, containment in strict

**Found by** repeating the best-model error audit across every model, and
aggregating the disagreements that recurred.

Three more:

- **Containment belonged in strict too.** "2 of the junior engineers" for a gold
  of "junior engineers", "clinical director at st vincent's" for "clinical
  director"; In each case the model was MORE faithful to the note than the label.
The note says "mentors two of the junior engineers". Strict was penalising
  models for being more specific than an under-specified gold.
- **Predicate equivalence was too thin.** Models write `joined` and
  `board_member` for `member_of`, `profession` for `has_role`, `met` for `knows`,
  `started_at` for attending, `located_in` for `lives_in`. Endpoints identical,
  predicate different, which the rules already permit; the table just did not
  cover them.
- **Honorifics and converse predicates**, above.

**Impact, and the useful part:** the leaders barely moved (26B unchanged at
0.920, 12B unchanged at 0.855) while the smaller models gained 0.02-0.05.
Weaker models reach for non-canonical predicates more often, so a thin
equivalence table was penalising them specifically. That is a *rank-distorting*
bias, not a level shift, the same defect class as the incomplete gold, pointing
the other way.

Every credit granted by the new rules was audited individually. All legitimate;
no over-crediting.

## 12. The confidence floor was doing the measuring: and it overturned the headline

**Found by** checking two harness parameters that had never been validated: the
token cap and the confidence floor. The cap was fine (only LFM2's repetition loop
reaches it; every other model peaks at 254 tokens against a 512 limit). The floor
was not.

`MF_CONF_FLOOR` (0.6) discards any fact below the threshold. Scoring with it
applied conflates two different things, whether a model can extract the fact,
and whether it emits a usable confidence, and the conflation falls almost
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
usable fact" was reported repeatedly across this whole exercise; it is false.
Qwen3-0.6B extracts at 0.400 F1 and every one of those facts is correct. The
floor throws all 72 away because the model writes `confidence: 0.0`.

The floor is a config value we choose, not a property of any model. The top four
models are unaffected by it (±0.011), so it was invisible in exactly the place I
was looking hardest.

**The subtlety worth keeping:** it is not a uniform penalty. gemma-4-E2B scores
*lower* without the floor (0.641 -> 0.575), so for that model the floor works as
designed, filtering low-confidence noise. It is a precision filter that some
models benefit from and others are destroyed by.

Both numbers are now reported: **capability** (floor lifted, can the model do
the task) and **committed** (floor applied, what the drain writes today). The
gap between them is a config decision, and for the small models it is the whole
story.

## 13. Correctness pass: bad gold found by consensus, and the scorer finally tested

Two angles not tried before.

**Gold triples no model produces.** If 23 models all miss the same labelled fact,
the label is the likelier error. Exactly one triple qualified:
`am05 ingrid|has_role|manager` from "My manager's manager is Ingrid." The models
produced ten different readings, none more than twice, none matching the label.
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

All pass. The single failure was in a test assertion, not the scorer, comparing
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
  **zero violations**. Worth knowing how much the rules carry. They add 3 to 11
  true positives per model, and granite-4.0-h-1b gains 46% (24 -> 35). Rules
  doing that much work had better be tested, which until now they were not.

**Found:**
- *The completeness guard was in the wrong place.* It lived in score.py's
  main(), so every ad-hoc analysis importing the module skipped it. An
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
does not, yet.

## Known gotchas that are NOT fixed

Things a reader or a rerun will hit. None are bugs; all are limits.

**The scoring rules were fitted to this data.** This is the biggest one and it
has no clean fix here. Predicate equivalences, aliases and containment were added
*because models produced them*. I read the disagreements and decided which were
unfair. Measured against exact-match-only, those rules account for **6-13% of
every top model's F1**:

| model | exact only | with rules | attributable to rules |
|---|---:|---:|---:|
| gemma-4-26B-A4B | 0.809 | 0.926 | 13% |
| gemma-4-12B | 0.769 | 0.862 | 11% |
| gemma-4-E4B | 0.656 | 0.738 | 11% |
| Qwen3.6-27B | 0.820 | 0.906 | 10% |
| Qwen3.6-35B-A3B | 0.746 | 0.791 | 6% |

Each rule is individually defensible, `met` does mean `knows`. But they
were chosen with the answers visible, which is the definition of fitting the
grader to the test set. The proper control is a held-out set of notes the rules
were never tuned against; it does not exist here. Anyone reproducing this should
treat the absolute numbers as generous by roughly a tenth, and note that the
effect is uneven (6% to 13%), so it distorts gaps as well as levels.

**One author, no independent review.** Every gold label was written by one model
(Claude Opus 5) and audited by the same one. The consensus check in defect 13
helps (23 models agreeing against a label is real evidence) but it cannot catch
a mistake the models share.

**n=69.** Differences under about 0.05 F1 are not meaningful. Several adjacent
pairs in the table are inside that.

**Categories with no gold triples report null, not 0.** transient, and most of
ambiguous and negation, have no gold triples, so P/R/F1 are undefined there.
Reporting 0.0 inverted the meaning, fp=0 on a factless note is perfect
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
moved in the same direction**. The models were better than measured, every
time. A grader with unbiased noise would have erred both ways.

That one-sidedness was itself the clue, and it is what prompted the tenth: if
every fix helps the models, the fixes are probably overshooting somewhere. They
were. Looking specifically for over-crediting found it immediately. The lesson is
not "audit your grader" but something narrower and more useful: **audit it in the
direction your corrections have not been going.**

The practical lesson for anyone building an eval: the failure mode is the grader being wrong in ways
that correlate with model quality, and a benchmark cannot detect that about itself. Every defect
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
scored bit-identical to the first run, 0.8550 / 0.8750 / 0.8358, 64 triples,
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
against the runner's **default 512-token cap**, a sixteenth of production's
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
and I reported it as "works, clearly worse", with its reasoning suppressed.

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
wrapper first, killed that, and left the real sweep running, which is how two
sweeps ended up serving the same port a second time.

Cost: one wasted .254 sweep, two interrupted sessions, and the invalid Magistral
run above.

The fix is that a sweep should be stoppable by its own lockfile rather than by pattern-matching a
process table. Another kill loop would not do it.

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
the heal loop deleted it, while the runner was still writing.

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
`llama-server --list-devices` enumerated exactly one device (the iGPU) and
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
any card, a ~19GB Q6 model against 8GB of shared memory on a swapping host
explains all three without any architecture bug. GLM subsequently ran clean on
`.253` under CUDA at 10.7 tok/s and scored **F1 0.7801**, the best recall in the
benchmark. The Vulkan/RADV theory was invented to explain a fit problem.

**Magistral-Small-2509 "too slow to finish".** 19 minutes per note in `D` state
is what thrashing looks like, not what the model costs.

`gemma-4-12B-it.q6` at 0.8630 stays in the index because accuracy plausibly
survives (the same GGUF answers the same wherever its tensors sit) but its
speed and fit numbers are void.

### The general lesson

Every sweep records a `device.json`, which was supposed to prevent exactly this.
It recorded the *requested* placement (`-ngl 99`) and the *absence of CPU-offload
warnings*, and both were satisfied by an iGPU. Provenance that cannot distinguish
two devices is not provenance.

`sweep_quant.sh` now pins `--device` explicitly and records `device_used`
alongside `device_requested`, because a host with more than one GPU will silently
give you the wrong one and every number will look plausible.
