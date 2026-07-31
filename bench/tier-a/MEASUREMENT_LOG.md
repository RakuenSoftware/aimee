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
