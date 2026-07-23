## 1. Objective

> Route each delegate packet to the **cheapest agent whose capability meets the packet's
> requirement**. Capability is a *filter*; price is the *ordering* among survivors.

Stated as a rule: threshold, then minimize. Deliberately NOT a scalar value score — see §3.2.


### 3.3b Operator-stated invariants (these outrank cost)

Stated by the operator 2026-07-22, in priority order above any cost objective:

1. **No outages.** Availability is paramount. Routing must not fail a request because no agent
   qualified.
2. **User-facing sessions must be coherent.** A user must never be handed a model too weak to
   hold a reasonable interaction. This is about the capability of the seat the user talks to,
   not about model stability across turns.
3. **The managing agent** — the orchestrator that decomposes work and dispatches other agents —
   must also run on the most capable tier. Its errors multiply across every packet it creates.
4. **Never route a packet to a model that cannot complete it** (the operator's phrasing: do not
   send to Luna what needs Sol).

Cheapest-with-capability (§1) therefore applies to **bounded delegate packets**, not to the
user-facing turn and not to the orchestrator.


## 7. Relationship to the economizer safety spec

`provider-neutral-economizer-safety-spec.md` is APPROVED and normative. Its baseline pins "the
same model, endpoint, account/project routing, service tier, region, and API shape", and its
rule is that Aimee does not intervene unless both user charge and authoritative provider cost
are provably strictly lower for the complete task lifecycle.

The argument that this proposal sits outside that gate:

- The spec constrains **interventions that claim savings**. This proposal's net effect on spend
  is *upward*: today's routing is unconditionally cheapest (§2.3), and adding a competence floor
  can only move packets to equal-or-more-expensive agents. A change that never claims a saving
  cannot fail a "must be strictly lower" test.
- One dispatch per packet, no discarded attempts, so there is no retry amplification to account
  for.

This argument was NOT accepted at round 1 of review, which raised a blocking finding that
"user/operator-selected policy" was an unfalsifiable relabeling of the forbidden intervention.
The reframing above is materially different — it turns on the *sign* of the cost effect rather
than on who selected the policy — but it has not yet been re-reviewed.

**This proposal must not ship until the panel accepts §7 or the spec is formally amended.**

