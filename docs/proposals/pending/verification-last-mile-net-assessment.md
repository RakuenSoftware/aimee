# Proposal: Verification last-mile net assessment

- **State:** PENDING ACTIVATION EVIDENCE. The bounded implementation is present;
  shadow calibration, an enabled document-ingress rollout, and executable-trial
  results remain before the proposal can be reconciled as done.
- **Date:** 2026-08-28
- **Owners:** workflow/lifecycle, learning/eval, KB ingress, and skills
  maintainers for their respective slices
- **Related:**
  [Aimee development lifecycle](../done/aimee-dev-lifecycle-workflow.md),
  [recursive self-improvement](recursive-self-improvement-closing-the-loops.md),
  [local-first memory and trust patterns](local-first-memory-and-trust-patterns.md),
  [structured PDF ingestion](../done/structured-pdf-ingestion-and-evidence-layer.md),
  [document lifecycle contract](../done/evidence-lifecycle-p3-document-lifecycle-contract.md),
  [current-stack ROI experiment suite](current-stack-roi-experiment-suite.md), and
  [skills module](../../modules/skills.md)

## Decision

Adopt two narrow verification improvements now, develop one document-ingress
control as a security slice, and require evidence before funding a larger skill
evaluation system:

1. **P0: blocker-set convergence.** Decide whether review is making progress
   from normalized blocking findings, not only from byte-identical artifact and
   feedback hashes.
2. **P0: activate admitted regressions.** Select relevant, already-admitted
   regression tasks during workflow verification. Extend the existing
   candidate ledger and eval harness; do not create a second scheduler or suite.
3. **P1: structural document-channel inspection.** Compare document structure,
   extracted text, and visibly rendered text before untrusted rich documents can
   enter prompts or durable knowledge.
4. **P2/conditional: executable skill evaluation.** Replace prerecorded response
   fixtures as promotion evidence only after a bounded experiment proves that
   executable held-out trials catch meaningful defects at acceptable cost.

Do **not** build signed execution receipts, another WORM or audit store, another
verdict bus, another memory layer, or fleet-coordination machinery. Aimee already
has the governing lifecycle and evidence substrate. Duplicating it would add
reconciliation failure modes without improving the decision boundary.

## Net assessment

The transferable value is **real but not foundational**. The useful residue is
four missing decision rules inside systems Aimee already owns. Two are high
confidence and relatively bounded. The document control is worthwhile because
Aimee already accepts PDF, DOCX, PPTX, HTML, RTF, and other rich formats, but it
has parser and maintenance cost. The skill-evaluation idea is plausible, not yet
proven to be worth a new execution path.

The following allocation is a planning weight, not an empirical ROI claim:

| Slice | Expected value | Confidence | Cost/risk | Planning share |
| --- | --- | --- | --- | ---: |
| Blocker-set convergence | high | high | low-medium | 30% |
| Relevant admitted regressions | high | high | medium | 35% |
| Structural document inspection | medium-high security value | medium-high | medium-high | 25% |
| Executable held-out skill trials | conditional | medium | high and recurring | 10% |

The first two slices carry roughly two-thirds of the expected value and should be
independently shippable. The proposal fails if implementation turns them into a
platform rewrite.

### What was rejected

| Idea | Assessment | Reason |
| --- | --- | --- |
| Signed receipts for ordinary actions | reject | Existing turn-integrity transitions, target/argument digests, postconditions, lifecycle events, and WORM records already answer what was proposed, authorized, attempted, and observed. A second signed envelope does not make a false observation true. |
| New append-only audit system | reject | Aimee already has lifecycle events and the audit WORM. Two histories create disagreement and migration burden. |
| New verdict or event bus | reject | The module bus and workflow lifecycle already carry decisions. Add typed fields to the owner, not a parallel authority. |
| New memory/provenance layer | reject | Existing memory, KB evidence, and learning ledgers own these records. The missing work is admission and activation policy. |
| Fleet-wide policy coordination | defer indefinitely | No demonstrated Aimee problem requires it. Revisit only with a concrete multi-node consistency failure and measured operator cost. |

## Existing substrate and exact gaps

This proposal is intentionally a delta map.

| Concern | Existing owner | Existing behavior | Missing behavior |
| --- | --- | --- | --- |
| Review convergence | Go workflow family; `wfe_convergence` | Parks on repeated identical artifact plus feedback hashes and on a total round cap | Cosmetic edits and blocker substitution can appear as progress; there is no normalized blocker-set comparison |
| Regression learning | learning synthesis, `eval_candidates`, ordinary eval suites | Normalized failure signatures, distinct-session admission, permanent rejection, and retirement exist | Nothing selects and runs relevant admitted tasks as part of a later change's verification |
| Document safety | KB normalizer, poison gate, prompt sanitizer, PDF evidence and quarantine | Extracts several rich formats, detects lexical injection patterns, and sanitizes rendered prompt text | Does not inspect hidden format channels or compare raw structure, extracted text, and visible text before ingestion |
| Skill quality | skills resolver, lint/review/lifecycle, `skill eval` | Parses stored baseline/treatment response strings and applies substring checks | Does not execute equivalent baseline and treatment trials on held-out cases |
| Action evidence | turn integrity, lifecycle events, audit WORM | Records bounded action transitions and effects | No gap addressed by this proposal |

The accepted design principle is **one owner per decision**. Each slice extends
the module named above and emits through existing lifecycle/audit paths.

## Conceptual basis

The design draws on established ideas, not on another product's architecture:

- **Monotone progress:** a process may claim demonstrated progress when the set
  of unresolved obligations strictly decreases. Equality is stasis; growth or
  substitution is not demonstrated progress.
- **Selective regression testing:** choose the tests affected by a change while
  preserving explicit safety conditions. Empirical work on safe regression test
  selection also warns that cost and benefit vary with suite design, so selection
  must be measured against run-all and missed-fault controls
  ([Rothermel and Harrold, 1998](https://digitalcommons.unl.edu/csearticles/11/)).
- **Treat retrieved content as untrusted input:** NIST's Generative AI Profile
  calls for measuring security controls and red-teaming prompt injection rather
  than treating provenance metadata as sufficient
  ([NIST AI 600-1](https://doi.org/10.6028/NIST.AI.600-1)).
- **Inspect the source format, not just extractor output:** Office Open XML is a
  package of standardized vocabularies and relationships, including formatting
  state that a plain-text converter need not preserve
  ([ECMA-376](https://ecma-international.org/publications-and-standards/standards/ecma-376/)).
- **Objective, repeatable evaluation:** test sets, tools, metrics, deployment-like
  conditions, and independent review should be documented; each measurement
  should add unique information
  ([NIST AI RMF, Measure](https://airc.nist.gov/airmf-resources/airmf/5-sec-core/)).

## Slice 1: blocker-set convergence

### Problem

`wfeRecordRequestedChanges` increments the no-progress counter only when both
the artifact hash and feedback hash are unchanged. Any textual change resets
that counter. This correctly catches a frozen loop but misses three common
non-progress forms:

- the artifact changes cosmetically while every blocker remains;
- one blocker disappears and a different blocker of equal weight appears; or
- reviewer wording or ordering changes while the underlying obligations do not.

The total iteration cap eventually parks the work, but only after spending the
full review budget and with a weaker explanation.

### Contract

Every blocking finding supplied to the convergence owner gets a deterministic
fingerprint from structured fields:

```text
fingerprint = H(
  schema_version,
  gate,
  reviewer_lens,
  severity_class,
  normalized_category,
  canonical_location,
  normalized_obligation
)
```

Normalization case-folds text, collapses punctuation and whitespace, resolves
repository-relative locations, sorts and deduplicates fingerprints, and rejects
oversized or malformed fields. An agent-supplied finding ID is evidence but is
not the identity key. The algorithm and schema version are fixtures shared by
the finding producer and workflow owner.

For previous blocker set `P` and current set `C`:

| Relationship | Classification | Counter effect |
| --- | --- | --- |
| first observation | baseline | initialize |
| `C` is empty | resolved | normal gate pass owns advancement |
| `C` is a strict subset of `P` | demonstrated progress | reset no-progress counter |
| `C = P` | stalled | increment no-progress counter |
| `C` is a superset of `P` | regression | increment no-progress counter and record additions |
| sets overlap but neither contains the other | churn/unproven | increment no-progress counter and record removed plus added |

This policy is deliberately conservative: a fixed defect may expose a new one.
Such a round is not declared failure; it is declared **unproven progress** and
receives the same bounded retry budget. A later strict decrease resets the
counter. The existing total-iteration cap remains the final bound.

The transaction that records a requested-change result atomically updates:

- last canonical blocker set and digest;
- consecutive rounds without demonstrated progress;
- relationship classification and added/removed fingerprints;
- existing artifact and feedback hashes; and
- the existing lifecycle event.

No model judges whether two findings are equivalent inside the enforcement
transaction. If structured finding fields are unavailable, the gate retains
the current hash-based behavior and records `blocker_set_unavailable`; it may not
silently treat missing structure as progress.

### Failure and abuse cases

- Reordering, capitalization, punctuation, and path spelling do not create new
  obligations.
- A reviewer cannot keep a loop alive by changing prose around the same
  structured category and location.
- A producer cannot collapse two blockers by reusing an ID.
- A hash collision or unknown normalization version fails to the current
  conservative round/identical-hash limits.
- Findings that genuinely lack a location remain distinguishable through lens,
  category, severity, and normalized obligation; they do not receive an invented
  source path.

### Acceptance

1. Equal blocker sets in different order and wording increment no-progress.
2. A strict subset resets no-progress even when the artifact hash also changes.
3. A swap, superset, or added blocker cannot reset no-progress.
4. A newly exposed blocker remains retryable and is reported as churn/unproven,
   not falsely called resolution.
5. Concurrent observations serialize per work item and gate.
6. Existing max-iteration, pause, override, cost, and event behavior remains
   compatible.
7. Migration from rows without blocker sets is deterministic and requires no
   second convergence table.

## Slice 2: relevant admitted regressions

### Problem

The learning path can turn repeated failures from distinct sessions into
ordinary admitted eval tasks, reject a poisoned candidate permanently, and
retire a long-passing task. The residual is operational: admitted regressions
do not automatically participate in verification of later relevant changes.

Running every learned regression on every change would be safe but expensive
and would eventually make admission self-defeating. Running none wastes the
ledger. The missing component is a conservative selector with visible recall and
cost behavior.

### Contract

Add an `admitted-regression-select` stage to the existing verification planning
path. It reads only ordinary tasks whose candidate state is `admitted`. It never
executes `candidate`, `rejected`, or `archived` records.

Selection is deterministic at a frozen repository revision and records a reason
for each selected task. Evidence ranks from strongest to weakest:

1. exact source path, symbol, route, schema, config field, or wire-kind named in
   task provenance intersects the change blast radius;
2. code-graph ownership or dependency reachability connects the change to that
   provenance;
3. the task belongs to an explicitly declared module or workflow verification
   suite changed by the patch; or
4. an operator-maintained always-run critical class applies.

Free-text embedding similarity alone never makes a task blocking. It may propose
an advisory candidate for later calibration.

The selector emits:

```text
selection_manifest = {
  repository_revision,
  diff_digest,
  selector_version,
  graph_snapshot_digest | unavailable,
  selected: [{task_digest, reason, matched_fact}],
  excluded_counts_by_reason,
  incomplete_reasons
}
```

Selected tasks run through the existing eval harness and settle through the
existing workflow result and lifecycle event paths. A selected admitted
regression failure blocks the verification stage exactly as a declared
repository test does. Replay of the same manifest is idempotent at the workflow
level even if the task is retried internally.

### Safety policy

- Admission remains upstream and cannot be bypassed by the selector.
- Permanent rejection wins over a stale task file.
- If exact provenance matches but graph service is unavailable, exact matches
  still run and the manifest records reduced selection coverage.
- If neither exact provenance nor a valid graph snapshot is available, the
  selector reports `selection_incomplete`; it does not invent broad blocking
  matches.
- During rollout, selection runs in observe-only mode. Blocking activates only
  after shadow runs measure false exclusion, added cost, flake rate, and faults
  caught against a run-all sample.

### Acceptance

1. A relevant admitted task is selected with a stable, inspectable reason.
2. An unrelated admitted task is excluded deterministically.
3. Candidate, rejected, and archived tasks never run, including when a stale
   suite file exists.
4. A selected task failure blocks only after the blocking rollout gate opens.
5. Graph outage produces explicit degraded coverage, not an empty success.
6. Identical revision, diff, candidate states, and graph snapshot produce the
   same manifest digest.
7. Shadow validation compares selected vs run-all on a preregistered sample and
   reports selection rate, missed failures, wall time, and realized provider
   cost before activation.

This slice is a narrow residual amendment to the existing recursive
self-improvement proposal. It does not re-own failure synthesis, admission,
rejection, task format, eval execution, or retirement.

## Slice 3: structural document-channel inspection

### Problem

Aimee's normalizer sends several rich formats through conversion tools and then
applies text-level integrity controls. This is necessary but incomplete. A rich
document may contain text in hidden runs, off-canvas objects, notes, alternate
text, comments, relationships, metadata, white-on-white styling, collapsed HTML,
or other channels that are absent from ordinary rendering but present in an
extractor's output or object model.

The security question is not merely whether suspicious words exist. It is
whether an untrusted instruction is concealed from the human-visible artifact
while remaining available to an agent.

### Contract

Insert bounded, static structural inspection before conversion and before any
document bytes or extracted text enter a prompt, KB row, embedding queue, or
learning path.

For each supported format the inspector produces:

```text
document_channel_report = {
  format,
  raw_digest,
  inspector_version,
  extractor_version,
  visible_text_digest,
  extracted_text_digest,
  hidden_spans: [{channel, location, reason, text_digest, lexical_verdict}],
  external_relationships,
  active_content_flags,
  resource_limit_verdict,
  disposition
}
```

The inspection path must:

- use static parsing only: no macros, scripts, field updates, external fetches,
  fonts, or embedded object execution;
- cap archive members, nesting, expanded bytes, compression ratio, XML nodes,
  relationships, images, pages, object count, output bytes, CPU, memory, and
  wall time;
- derive an approximation of human-visible text separately from the converter's
  extracted text;
- classify all non-visible or alternate channels and pass their text through the
  existing lexical integrity gate; and
- preserve only digests and bounded diagnostic snippets in lifecycle/audit
  records, honoring the document's sensitivity class.

### Disposition

| Condition | Result |
| --- | --- |
| clean structure and no suspicious hidden text | admit through existing ingest path |
| hidden content with no imperative/injection signal | quarantine or warn according to source policy; never silently ignore |
| hidden content plus imperative/injection signal | reject or owner-review quarantine before model/KB exposure |
| active content, external relationship requiring fetch, parser ambiguity, resource-limit breach, or unsupported structure | fail closed for untrusted sources |
| visible text contains a current lexical-gate match | preserve existing poison-gate disposition |

Structural hiding alone is not proof of malice: legitimate documents use notes,
tracked changes, accessibility text, and conditional content. Conversely, a
visible instruction can still be hostile. The combined structural and lexical
rule improves precision without pretending to solve prompt injection generally.

### Scope and sequence

1. **OOXML and HTML first:** DOCX, PPTX, XLSX, and HTML have inspectable package
   or DOM structure and are already accepted by Aimee.
2. **PDF second:** integrate hidden/off-page/font/render-state checks with the
   existing coordinate-aware PDF path rather than adding another PDF parser.
3. **RTF/ODT/EPUB only after format-specific fixtures exist.** Until then,
   untrusted instances use explicit unsupported/quarantine policy instead of a
   generic claim of structural safety.

Generated rich-document egress may compare hidden-span digests against ingress
reports to detect propagation, but this is an advisory extension. It does not
block the ingress slice and it is not a data-loss-prevention product.

### Acceptance

1. Fixtures cover hidden runs, white-on-white text, off-canvas objects, notes,
   comments, alt text, metadata, collapsed DOM nodes, external relationships,
   malformed packages, and archive bombs for every enabled format.
2. Hidden benign text and hidden imperative text receive different dispositions.
3. No active content or external relationship executes or fetches in tests.
4. Converter output cannot bypass a rejecting structural report.
5. Parser timeout, crash, overflow, unknown version, or ambiguous rendering fails
   closed for untrusted content and leaves no partial KB rows.
6. Re-ingest is idempotent by raw digest plus inspector policy version.
7. Sensitivity and quarantine behavior remains owned by the existing document
   lifecycle; there is no new document store.

## Slice 4: executable held-out skill evaluation

### Problem

Current `skill eval` fixtures contain both `baseline_response` and
`treatment_response`. The evaluator applies deterministic checks to those stored
strings. This is useful fixture linting, but it does not establish that loading a
skill changes an agent's behavior. The author can accidentally or deliberately
write the answer into the fixture.

Executable comparison is more credible, but it introduces provider cost,
nondeterminism, tool side effects, test leakage, and grader dependence. It should
not become a core promotion dependency until a pilot demonstrates incremental
defect detection.

### Pilot contract

Keep the current stored-response command as `skill eval-fixtures` compatibility
behavior. Add an opt-in executable mode with a frozen manifest:

```text
skill_trial_manifest = {
  skill_digest,
  held_out_case_set_digest,
  baseline: skill_absent,
  treatment: skill_present,
  model_and_route,
  tool_contract_digest,
  prompt_and_policy_digests,
  seed_policy,
  repeats,
  per_case_and_total_budget,
  graders,
  promotion_thresholds
}
```

Baseline and treatment receive the same model, tools, context, budgets, and case
input. Trial order is balanced AB/BA. Deterministic graders are preferred:
schema validation, exact properties, repository tests, forbidden tool calls,
effect absence, or success against a controlled fixture. Model-backed judging
is allowed only as a separately reported secondary measure with repeated trials
and a frozen judge configuration.

Held-out cases are inaccessible to the skill-authoring run. Imported skill bytes
do not import a trust verdict: the receiving installation runs its own lint,
policy, and executable evaluation under local tools and permissions.

No executable skill trial may mutate the operator's live project or contact an
external party. It runs in an isolated worktree/sandbox with deny-by-default
tools. A skill that requests destructive, credential, deployment, or outbound
capabilities remains human-gated regardless of its score.

### Funding gate

Run the pilot on a preregistered set of known-good, known-bad, overfit, and
no-effect skills. Fund production promotion wiring only if executable trials:

- catch materially more known defects than lint plus stored-response fixtures;
- keep false rejection and flake within declared bounds;
- demonstrate repeatable effect sizes across at least two supported model/route
  conditions; and
- fit a declared provider-cost, latency, and operator-review budget.

If the pilot fails, retain fixture linting and human review. Do not maintain an
expensive harness for a theoretical assurance gain.

### Acceptance for the pilot

1. Baseline and treatment differ only by skill inclusion and balanced order.
2. Full call, tool, retry, and grader cost is attributed to the trial.
3. Held-out case contents are absent from authoring context and skill support
   files.
4. A no-effect skill cannot pass on treatment quality alone; it must beat its
   paired baseline by the preregistered threshold.
5. A trial that times out, exceeds budget, loses settlement data, or attempts a
   denied effect is inconclusive/fail, never pass.
6. Revalidation uses the locally installed skill digest and local policy.
7. The pilot report makes an explicit ship/stop decision; it does not silently
   turn the experiment into permanent infrastructure.

## Shared ownership and evidence rules

Each slice records decisions through the existing owner:

- workflow convergence through `wfe_convergence` and `lifecycle_event`;
- regression selection through eval task/candidate records and workflow
  verification results;
- document disposition through the existing ingest, sensitivity, quarantine,
  and evidence lifecycle; and
- skill trial state through the existing skill review/lifecycle records.

Where governed execution occurs, existing turn-integrity effect and WORM paths
remain authoritative. New rows may store the structured fact needed by their
owner, but no slice writes a second general-purpose audit narrative.

Evidence records bind canonical input digests, policy/schema version, decision,
reason, and result. Cryptographic signatures are not required for records that
remain inside the same trusted Aimee installation. If a future cross-trust export
needs origin authentication, sign the exported manifest at that boundary; do not
redesign every internal event around that hypothetical.

## Delivery order and stop rules

### P0-A: blocker convergence

Land normalization fixtures and shadow classifications first. Then migrate the
single convergence table and activate the no-progress counter. Stop if finding
producers cannot provide stable structured obligations without model judgment;
retain the current total-round bound rather than pretending prose hashes are
semantic identity.

### P0-B: regression activation

Add provenance completeness, deterministic selection manifests, and shadow
run-all comparisons. Activate blocking only for exact or graph-proven matches
after missed-failure and cost thresholds are accepted. Stop expansion if the
suite's provenance is too weak; fix provenance rather than compensate with broad
similarity.

### P1: document inspection

Deliver formats one at a time behind an ingest policy flag. A format is not
declared structurally inspected until its adversarial fixture matrix and
resource-limit tests pass. Unsupported untrusted formats remain quarantined or
rejected according to policy.

### P2: skill trial experiment

Build only the bounded experiment. Promotion integration is a separate approval
after the funding gate.

The slices do not depend on one another and should not share a release flag or
schema migration.

## Implementation status

The implementation deliberately stops at the activation boundaries specified
above:

- Review feedback now carries deterministic, ID-independent blocker
  fingerprints into the existing convergence row. `observe` records strict
  subset, equality, growth, and churn without changing routing; `enforce` makes
  only strict shrinkage count as progress and retains the prior exact-hash rule
  whenever structured blockers are unavailable.
- Workflow implementation steps now deterministically select admitted
  regression tasks by explicit provenance path, origin reference, or an exact
  path token in the held task. The manifest binds the repository revision, diff
  digest, selector version, selection reasons, and known incompleteness. Observe
  mode is configured in the standard change workflows. Enforce mode runs only
  ledger-identical admitted task bytes and fails closed on a possibly truncated
  candidate scan; code-graph expansion remains explicitly unavailable.
- KB HTTP ingestion has an in-process, resource-bounded HTML and OOXML
  structural inspector before conversion or staged writes. It detects hidden
  channels, active content, external relationships, malformed packages, nested
  archives, and decompression limits, then reuses the existing lexical integrity
  gate for concealed text. `AIMEE_KB_DOCUMENT_INSPECTION` is off when unset, so
  rollout requires an explicit operator decision. Unsupported rich formats are
  rejected while the flag is enabled rather than being described as inspected.
- `aimee skill eval-fixtures` preserves stored-response compatibility, while
  `aimee skill eval-exec` runs the bounded pilot against operator-held cases in
  `.aimee/skill-evals/<name>`. It uses paired tool-free calls on one frozen
  route, balanced order, hard call/output/spend limits, deterministic graders,
  and content-addressed manifests. Unknown cost, route drift, attempted effects,
  runner errors, or budget excess are inconclusive. No result changes production
  promotion state.

This proposal remains pending because implementation is not activation
evidence. The blocker and regression controls need shadow measurements, rich
document inspection needs an explicitly enabled rollout, and executable skill
trials need real cost and defect-detection results before any promotion hook is
funded.

## Compatibility and migration

- Existing workflow rows without blocker-set fields keep current behavior until
  their next structured observation. No historical finding reconstruction.
- Existing eval candidates and task files retain state and format. New provenance
  fields are additive; missing provenance means not eligible for blocking
  selection, with an explicit diagnostic.
- Existing document ingestion remains available for trusted local sources during
  rollout. Untrusted-source policy becomes stricter only per enabled format and
  must be called out in release notes.
- Existing `skill eval` fixture behavior remains callable and is relabelled
  honestly; no stored fixture is auto-converted into an executable held-out case.
- No WORM history, lifecycle event, memory, or KB evidence migration is created
  merely to add signatures.

## Proposal-level acceptance

```yaml acceptance
- id: 1
  tier: mechanical
  check: "python3 scripts/check-proposal-links.py"
- id: 2
  tier: mechanical
  check: "python3 scripts/check-proposal-reconcile.py"
- id: 3
  tier: mechanical
  check: "python3 scripts/check-docs.py"
```

Implementation is complete only when each funded slice satisfies its own tests,
its shadow/experimental activation gate, and the existing owner's integration
checks. Shipping one slice does not imply acceptance of the other three.

## Non-goals

- proving that a logged observation is factually true;
- making model text a cryptographic trust root;
- replacing human approval for destructive or external effects;
- executing every regression on every change;
- detecting every form of prompt injection or document deception;
- treating hidden content as malicious without context;
- building a general document renderer, DLP product, skill marketplace, or
  distributed trust protocol; or
- claiming ROI before measured activation, caught-fault, and realized-cost data
  exist.
