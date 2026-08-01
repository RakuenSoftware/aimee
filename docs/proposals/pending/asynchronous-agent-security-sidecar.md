# Proposal: Optional asynchronous security integration

- **State:** DRAFT.
- **Date:** 2026-07-21.
- **Scope:** Public, model-agnostic integration contract only.

## Purpose

Aimee may support an optional private security service that observes authorized shadow copies of
agent activity and can request containment through existing Aimee control mechanisms. The service
is asynchronous: it is not placed on the connection, request, tool, model, or final-effect critical
path, and normal Aimee operation does not depend on receiving a model decision.

This proposal does not claim that arbitrary text can be classified as safe. Deterministic identity,
capability, sandbox, information-flow, approval, and final-effect controls remain the prevention
boundary. Private detection is defense in depth and must not become an authorization oracle.

## Public integration boundary

The public repository may provide only reusable, detector-independent infrastructure:

- an opt-in, disabled-by-default, non-blocking shadow interface;
- opaque and versioned observation envelopes;
- bounded enqueue behavior that drops shadow work instead of delaying primary work;
- authenticated attachment of an independently deployed private service;
- generic containment requests that are reauthorized by the control plane;
- generic acknowledgements, coverage state, health, queue/drop accounting, and audit references;
- lifecycle and cancellation seams that are useful without a detector; and
- deterministic mocks and tests proving non-interference and authority separation.

The public interface exposes no detector reason, score, feature, internal evidence, or actionable
explanation to an observed user. Public errors remain generic and must not provide an oracle for
probing private behavior.

## Required properties

1. **Asynchronous operation.** Mirroring and analysis cannot block, rewrite, steer, or delay normal
   traffic. Backpressure sheds shadow work and records a generic coverage gap.
2. **No new authority.** The private service cannot directly mutate Aimee state. Every requested
   effect is authenticated, bounded, auditable, generation-bound, and independently authorized at
   execution time by deterministic control-plane policy.
3. **Fail-safe separation.** Absence, failure, overload, compromise, or removal of the private
   service cannot bypass deterministic platform controls or corrupt primary traffic.
4. **Least observation.** Capture is explicitly enabled, tenant-scoped, purpose-bound, minimized,
   revocable, and subject to retention and deletion policy.
5. **Secret exclusion.** Structured credentials, authentication material, private keys, vault
   plaintext, and other prohibited fields are removed before any private-service boundary.
6. **Tenant isolation.** Observation, learned state, evidence, commands, and audit references cannot
   cross tenant or installation authority domains.
7. **Bounded containment.** Automated containment is temporary and limited to authority already
   granted by public policy. Broad or durable action requires ordinary administrative approval.
8. **Observable coverage.** Missing, dropped, stale, or unverified observation is represented as
   unknown coverage, never silently treated as safe.
9. **Removability.** An installation that does not enable the option has no runtime dependency on
   private detector infrastructure.

## Private specification boundary

All implementation and security-sensitive material is maintained outside the public repository,
including:

- model architecture, weights, runtime, prompts, tokenization, features, signals, and detector
  inputs;
- attack indicators, signatures, taxonomies, evidence codes, findings, heuristics, and response
  selection logic;
- training, labeling, calibration, distillation, evaluation, red-team, replay, and self-learning
  mechanisms;
- datasets, generators, fixtures, benchmarks, expected outputs, scores, thresholds, quality
  measurements, holdouts, and failure analysis;
- exact schemas, cryptographic profiles, key roles, transport details, private protocol negotiation,
  and release-attestation internals not required by the generic public attachment seam;
- exact capacities, queue allocations, memory limits, timing windows, leases, retries, rate limits,
  sampling, admission policy, overload behavior, and response budgets;
- deployment topology, hardware envelopes, enabled capabilities, current readiness state, known
  gaps, operational tuning, and performance measurements;
- private artifact identities, hashes, registries, build manifests, compatibility data, and supply
  chain exceptions;
- operator evidence, incident data, investigation workflows, break-glass details, recovery
  procedures, and response playbooks; and
- source, tests, comments, documentation, dashboards, or logs that would help reproduce, predict,
  exhaust, probe, or evade private behavior.

The private specification is normative for enabled deployments. Public documentation may state a
nonrevealing compatibility or readiness result, but not the values, evidence, or reasoning behind
that result.

## Publication and history rule

Security-sensitive drafts must originate in the restricted repository. They must never be staged,
committed, attached to a public pull request, uploaded as a public CI artifact, copied into issue
text, or retained in public Git history. Movement from private to public requires an explicit
security review that proves the material is necessary for interoperability or independent review
and does not expose private behavior or deployment posture.

Automated repository checks should reject additions matching private artifact formats, detector
terminology, operational constants, attack fixtures, or restricted document identifiers. A failure
of that check blocks publication.

## Non-goals

- Publishing an implementation-ready detector or operational design.
- Replacing deterministic authorization, sandboxing, egress control, or final-effect enforcement.
- Synchronizing a model decision with every connection or action.
- Rewriting or sanitizing live traffic using detector output.
- Guaranteeing prevention of the first harmful effect observed asynchronously.
- Depending on secrecy for the correctness of deterministic platform controls.

