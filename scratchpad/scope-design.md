# Design for approval: task SCOPE and per-agent scope ceiling

## Operator requirement

"Our local delegates will be able to do some coding tasks, but the more complex ones
aren't possible." Less capable delegates must execute easier tasks; harder delegates
harder tasks. Today a local model registered at cost_tier 0 would win EVERY packet
under cheapest-first routing.

## Proposal

Two new declarations, filtering exactly like the existing capability gate.

1. `agent_t.max_scope` — the hardest work this agent may be given. Operator-declared,
   default UNBOUNDED so every existing config is unchanged.
2. Packet `scope` — set by the orchestrator when it decomposes work, default
   `whole_task` (conservative; boundedness is OPT-IN).
3. Routing filter: a packet whose scope exceeds an agent's ceiling excludes that agent.

Vocabulary (2 values, matching what observed delegate prompts actually distinguish):
  - `bounded`     : a specified, self-contained change - named file(s)/function, an
                    acceptance check the delegate can run. Observed shape: the
                    SWE-bench style "fixing a bug in django/django, file X".
  - `whole_task`  : the complete approved task in a worktree, repository verification,
                    fix failures. Observed shape: the `code` jobs.

## Why a declared CEILING rather than a measured competence score

- No prediction and no classifier: the operator knows their local model's limits.
- No data dependency: works on day one; benchmarks can refine the ceiling later.
- Same shape as machinery that exists (min_context in agent_satisfies_required_caps).
- Makes local models USABLE rather than excluded: they win the bounded work they are
  good at and are excluded from the rest.

## The correctness trap this design must avoid

Routing already FAILS UPWARD: an unsatisfiable `min_context` escalates to the most
capable seat rather than failing (a DEGREE shortfall). Scope must NOT behave that way.
If it did, a `whole_task` packet with only bounded-capable agents would escalate INTO
the weakest seat - the exact inversion of the requirement.

So scope binds like `required_caps` (KIND: cannot be relaxed, escalation still enforces
it), not like `min_context` (DEGREE: relaxable). If nothing can serve a whole_task
packet, that is a config error worth surfacing, not something to paper over.

## Who sets the packet scope

The ORCHESTRATOR, when it decomposes work - it already chooses the role, and per the
operator's invariants it runs on the most capable tier. So the labelling is done by the
most capable model in the fleet as a side effect of an action it already takes. No
classifier, no extra dispatch.

## Prior decisions this builds on (do not re-litigate)

- Escalation LADDERS are NOT banned outright — corrected by the operator: they are
  acceptable as an EMERGENCY path. What is rejected is a ladder as the FUNDAMENTAL
  routing mechanism: "we don't want the fundamental task ladder to be less capable ->
  capable -> more capable across 3+ delegates for all tasks; this should be a
  fundamentally rare occurrence."
  So the steady state is route-right-first-time (scope matched to ceiling, ONE
  dispatch); a ladder is a bounded exception, and its rate is a health signal rather
  than a normal cost of doing business.
- `code_simple`/`code_complex` as ROLES are rejected in favour of scope as routing
  METADATA: scope is auditable, difficulty is subjective, and difficulty roles multiply
  combinatorially.
- Capability routing is ON by default and fails upward.

## Operator principles governing this design (settled)

1. **Route right the first time.** Scope matched to a declared ceiling, ONE dispatch.
2. **Under uncertainty, OVER-SELECT.** Prefer the more capable seat. Over-spending is
   strictly better than needing a ladder: it costs money, whereas a ladder costs money
   AND wastes a full dispatch AND means placement was wrong.
3. **An escalation is a MISPLACEMENT INCIDENT, not a safety net.** "An escalation being
   called fundamentally means how we placed the work seriously fucked up." It must be
   recorded as a placement defect (delegate_learnings has failure_mode/lesson), surfaced,
   and fed back into scope labels and ceilings - never silently absorbed.
4. Therefore the ladder must stay RARE by construction, and its rate is a health metric
   on the placement logic, not a normal cost of doing business.

## Questions for the panel

1. Is a declared per-agent CEILING the right primitive, or is there a materially better
   one given no competence data exists yet?
2. Is 2 values right? Is there a third the observed data or general practice demands?
3. Is binding scope as KIND (non-relaxable during escalation) correct, and what should
   happen when NO agent can serve a whole_task packet - hard failure, or something else?
4. Default packet scope `whole_task` (conservative) vs `bounded` (cheap): confirm.
5. What breaks for an operator who declares max_scope on an agent that is ALSO their
   only agent, or their primary/default seat?
6. ESCALATION POLICY. Given a ladder is permitted only as a rare emergency, specify
   it concretely: what triggers it (a VERIFIED failure such as the existing
   cross_verify build/test gate, versus any failure), how many steps it may take
   before giving up, whether it may cross a scope ceiling it was just filtered on,
   and what rate of escalation should be treated as evidence that the scope labels or
   the declared ceilings are wrong rather than as normal operation. Propose the
   instrumentation that makes "rare" measurable rather than assumed.

7. Anything materially wrong or missing.
