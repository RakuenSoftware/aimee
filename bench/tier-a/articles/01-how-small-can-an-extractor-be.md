# How small can a fact extractor be

DRAFT. Every number below is measured on 69 notes. That sample size is the
central limit on this article and is addressed in its own section rather than in
a footnote. Open measurement items are listed at the end.

If you want a local model to turn a note into structured facts, the first
question is how much model you have to pay for. We measured sixteen, from 230M
to 4.5B effective parameters, on one task: read a short note, return
subject-relation-object triples as JSON, or return an empty list if the note
asserts nothing durable.

Two findings are usable today. Usable extraction starts lower than expected,
around 800M to 1B. And the two ways small models fail are different enough that
collapsing them into one score hides the signal you need.

## What was measured

Sixteen models, greedy decoding, one prompt, one gold set. Source:
`bench/tier-a/results/dataset.csv`, captured 2026-07-31. Configuration recorded
in `results/PROVENANCE.json`: prompt from `src/kb/kb_memory_facts.c`, ontology
from `src/rel_types.c`, `max_new_tokens` 512, confidence floor 0.6.

The gold set was 69 notes carrying 64 triples, 23 of them deliberately factless.

| model | params | F1 | JSON parse | schema | CPU ms/note |
|---|---|---:|---:|---:|---:|
| google/gemma-4-E4B-it | E4B (4.5B eff) | 0.748 | 1.00 | 1.00 | 35,230 |
| ibm-granite/granite-4.1-3b | 3B | 0.648 | 1.00 | 1.00 | 27,346 |
| unsloth/gemma-3n-E4B-it | E4B | 0.639 | 1.00 | 1.00 | |
| google/gemma-4-E2B-it | E2B (2.3B eff) | 0.593 | 0.97 | 0.99 | 17,985 |
| ibm-granite/granite-4.0-1b | 1B | 0.592 | 0.88 | 0.91 | 13,568 |
| Qwen/Qwen3-1.7B | 1.7B | 0.567 | 0.99 | 0.99 | 12,088 |
| ibm-granite/granite-4.0-h-1b | 1B | 0.507 | 0.97 | 0.97 | 12,428 |
| Qwen/Qwen3.5-0.8B | 800M | 0.438 | 1.00 | 1.00 | 6,747 |
| Qwen/Qwen3-0.6B | 600M | 0.403 | 0.99 | 0.97 | 6,095 |
| Qwen/Qwen3.5-2B | 2B | 0.324 | 1.00 | 1.00 | |
| ibm-granite/granite-4.0-350m | 350M | 0.206 | 0.84 | 0.84 | 3,257 |
| ibm-granite/granite-4.0-h-350m | 350M | 0.136 | 0.32 | 0.32 | |
| LiquidAI/LFM2-350M-Extract | 350M | 0.070 | 0.14 | 0.14 | 2,806 |
| LiquidAI/LFM2.5-230M | 230M | 0.026 | 0.65 | 0.65 | 2,233 |
| HuggingFaceTB/SmolLM2-360M-Instruct | 360M | 0.000 | 1.00 | 0.00 | 4,304 |
| unsloth/gemma-3-270m-it | 270M | 0.000 | 1.00 | 0.00 | |

F1 here is the unfloored figure. The next section explains why that choice
changes four of these rows.

## Two ways to score zero

Our first table showed four models at 0.000. They do not fail the same way, and
the difference decides whether a model is unusable or merely mishandled.

**SmolLM2-360M and gemma-3-270m emit valid JSON and never the right shape.**
JSON parse rate 1.00, schema rate 0.00. They answer, the answer parses, and it
is never the `{"facts":[...]}` wrapper the task asks for. No downstream gate
recovers that. Both are unusable for this task at this prompt.

**Qwen3-0.6B was not zero.** It scores 0.000 under the shipped confidence floor
of 0.6 and 0.403 without it. The model extracted facts, wrote a confidence of
0.0 beside them, and a gate meant to protect precision discarded all of them.
Across all sixteen models the self-reported confidence carries almost no signal:
most write exactly 0.0 or exactly 0.9 and nothing between. The floor has since
been retired and replaced with a groundedness check that both endpoints trace
back to the note text.

granite-4.0-350m shows the same pattern: 0.000 floored, 0.206 unfloored.

If you take one operational point from this article, take this one. A small
model that scores zero on your benchmark may be extracting correctly and failing
your gate. Check the unfloored score and the schema rate before concluding the
model cannot do the task.

## Bigger is not reliably better

One inversion here is a genuine size effect, inside a single family at identical
settings.

| family | smaller | larger |
|---|---|---|
| Qwen3.5 | 0.8B scores 0.438 | 2B scores 0.324 |

The 2B runs the wrong way by 0.114. We have no mechanism for it. It is reported
because it appeared consistently across the conditions we ran, not because we can
explain it. At 69 notes it is not resolvable, for the reason in the next section.

Two nearby comparisons look like the same finding and are not. Separating them
matters more than the headline does.

**granite-4.0-1b at 0.592 against granite-4.0-h-1b at 0.507 is not a size
result.** Both are 1B. The `h` is a hybrid architecture variant at the same
parameter count, so the 0.085 gap measures the variant, not the budget. It
answers which 1B to pick, not how much model to buy.

**granite-4.0-1b against granite-4.1-3b inverts only under the floor.** Floored,
the 1B wins, 0.600 against 0.571. Unfloored, the 3B wins, 0.592 against 0.648.
The floor discarded 13 of the 3B's facts against the 1B's 2 — the same mechanism
as Qwen3-0.6B above, in a model large enough that nobody thought to check it.
The pair measures the gate, not the parameter count.

So one size inversion survives out of sixteen models, and it sits well inside
the interval described next.

## What 69 notes can and cannot tell you

This is the disqualifying limit on everything above, and it belongs here rather
than at the end.

A later comparison on the same task, run at 1001 notes with paired bootstrap
resampling over 5000 replicates, produced 95% intervals of roughly plus or minus
0.03 on strict F1. Sample error scales with the square root of n, so at 69 notes
the comparable interval is near plus or minus 0.12.

Resolvable at this sample size:

- the gap between models producing no usable output (0.00 to 0.21) and models
  above 0.55
- the schema-rate failures, which are categorical rather than a difference of
  degree
- the CPU cost spread, which is a throughput measurement and not an accuracy
  estimate

Not resolvable at this sample size:

- granite-4.0-1b at 0.592 against gemma-4-E2B at 0.593
- either inversion above
- any ordering among the five models between 0.50 and 0.65

Those orderings are consistent with the data. They are not established by it.

We know this limit is real rather than theoretical because we walked into it. A
separate claim on this benchmark, that enabling the model's reasoning pass is
worth 0.084 F1, came from a 70-note measurement with no interval attached. It
became a constant quoted in three source files and shaped a design decision.
Re-measured at 955 notes it was 0.010, with an interval spanning zero.

## Cost is the other axis

CPU time per note spans 2,233 ms at 230M to 35,230 ms at E4B, a factor of 16.
These are estimates derived from measured generation and prompt throughput, not
wall-clock on a production host, and the dataset labels the column
`cpu_est_ms_per_note` for that reason.

granite-4.0-1b reaches 0.592 at 13,568 ms. gemma-4-E4B reaches 0.748 at
35,230 ms. You pay 2.6 times the CPU time for 0.156 more F1, and the 0.156 is
the number in that sentence with a wide interval on it.

A second cost appears in no accuracy column. On GPU, the largest quantisation of
E4B took 420 seconds to load before serving its first note. When you run several
instances in sequence, load time is a real part of the budget.

## What we would use today

For extraction on constrained hardware, granite-4.0-1b and gemma-4-E2B are the
first two to try, on the measured evidence that both sit near 0.59 at roughly a
third and a half of E4B's CPU cost. E4B is the accuracy choice where the budget
allows it.

Below 800M we have not measured a model that produces usable structured output
at this prompt. That statement is scoped to this prompt and this schema.
LFM2-350M-Extract is published for extraction work and scored 0.070 here with a
0.14 JSON parse rate, which points at a format mismatch rather than a settled
verdict on the model.

## What still needs measuring

Listed so the article does not imply more than the data supports.

1. Re-run the top six on the current corpus and ontology. Every number here
   predates fixes to the benchmark itself: a corpus template that mislabelled a
   deployment relation, an ontology that did not define 19% of its own gold's
   predicates, and a prompt clause that suppressed one model's reasoning pass
   entirely. Estimated 20 minutes per model on the current sharded harness.
2. Establish intervals at 1000 notes for any ranking stated here. The five
   models between 0.50 and 0.65 need it most.
3. Re-test LFM2-350M-Extract with a prompt matched to its expected format. A
   0.14 parse rate is a format problem, not a capability measurement.
4. Measure load time per model as a first-class column. It is currently observed
   only in passing.
5. Confirm the Qwen3.5 inversion or withdraw it. One family, two sizes, 1000
   notes, identical conditions.

Until item 1 lands, treat the rankings here as a hypothesis about order rather
than a measurement of level.
