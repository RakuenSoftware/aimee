# Role taxonomy: cull and redesign, grounded in observed delegate usage

## Observed usage (real, not inferred)

Sampled 40 delegate jobs spread across the full job-id range, 2026-07-18 .. 07-23.
Only FOUR role values appear in six days of production use:

| role | n | what the prompts actually ask for |
|---|---|---|
| review | 31 | almost entirely ONE shape: "Review the complete artifact against the complete original request. ARTIFACT STAGE: {plan\|frozen_diff}" — a staged artifact-vs-request gate. Plus a roundtable-chairman synthesis review and a proposal-to-plan review. |
| draft | 5 | benchmark harness work: "You are fixing a bug in django/django, file ..." (SWE-bench style), and a best-of-N patch selection. NOT drafting prose. |
| code | 2 | "Implement the complete approved task in this worktree, run the repository verification, fix failures" |
| roundtable | 1 | adversarial diff review (this is how the panel is invoked) |

`roundtable` is used in production but is declared NOWHERE: not in the role list,
not in role_templates, not in the alias table. It works only by falling through.

Roles with ZERO observed use in the window: explain, refactor, execute, summarize,
format, search, diagnose, validate, deploy, reason.

## Current declared state (four lists that disagree)

- CLI help: code, review, explain, refactor, draft, execute, summarize, format, search, diagnose, validate
- default_exec_roles (granted to every agent that declares no exec_roles): deploy, validate,
  test, diagnose, execute, review, code, refactor, draft, implement, continuity, prose,
  line-edit, beat-check, lyric, hook, prosody, songform
- role_templates: the above minus deploy/test/implement, plus reason
- util.c error hint: a fourth, different list

`test`->validate and `implement`->code are ALIASES yet appear in default_exec_roles.
`deploy` has no template and no route. `reason` has a template and two aliases
(rank-fuse, classify-score) but nothing routes to it.

## Behaviour classes the code already keys on

- write (grants workspace-write): code, refactor
- tools on by default: review, search, execute, diagnose, validate
- result-cacheable: summarize, format, draft
- forced final turn after N: validate

## Agreed cull (persona vs role)

Novel/songwriter are PERSONAS, not roles. Cross-referencing persona.c: songwriter
delegates NOTHING (policy "none", roles ""); novel is read-only and delegates only
continuity, beat-check, review, research. So these six are unreachable as delegate
roles and are being deleted outright (templates included): prose, line-edit, lyric,
hook, prosody, songform. continuity and beat-check stay for now — novel genuinely
delegates them and continuity is its done-gate check_role — though they are
persona-shaped (novel x review, novel x validate) and arguably belong in a persona
migration later.

## Questions for the panel

1. Given only review/draft/code/roundtable are actually used, should the unused
   canonical roles (explain, refactor, execute, summarize, format, search, diagnose,
   validate) be culled, kept as declared-but-unused vocabulary, or kept only where a
   behaviour class depends on them? Note routing FILTERS on role, so a role that no
   agent declares is not free: it changes which agents are eligible.

2. The dominant real workload is a STAGED ARTIFACT GATE (review of plan, then of
   frozen_diff, against the original request). Should that become its own role (or
   roles) rather than being flattened into `review`? The stage is currently passed in
   the prompt text, invisible to routing.

3. `roundtable` is in production use and undeclared. Declare it as a role, or is it
   properly an orchestration MODE that happens to reuse the delegate path?

4. What difficulty axis is worth encoding, given the tiered-routing work? The example
   offered was code_simple vs code_complex. Is splitting on difficulty the right axis
   at all, or is the useful axis something else the observed prompts suggest (e.g.
   bounded-single-file vs whole-task, or verification-gated vs not)?

5. `deploy` and `reason`: cull, or implement? deploy is listed by the engineer persona
   and in an error hint but has no template or route; reason has a template and aliases
   but no route.

6. Propose the FINAL role set and its taxonomy. Be concrete and minimal. Justify each
   role by either observed usage or a behaviour class that must key on it. Say plainly
   which of your recommendations are not supported by the observed data.
