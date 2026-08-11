package economizer

import (
	"math"
	"testing"
)

func validKey() RegistryKey {
	return RegistryKey{
		Provider: ProviderOpenAI, EndpointID: 1, ModelSnapshotID: 2, TokenizerID: 3,
		PricingTableID: 4, ContractVersions: 5, TransformID: 6, TransformVersion: 7,
		ScenarioSetID: 8, ScenarioCoverage: 0b11, // two scenarios: ids 0 and 1
	}
}

func cheapProof() *Proof {
	return &Proof{
		TenantID: 1, AccountID: 2, CallID: 3,
		RegistryGeneration: RegistryGeneration(),
		Transform:          validKey(),
		Scenarios: []Scenario{
			{ScenarioID: 0, BaselineLower: 1000, BaselineUpper: 2000, CandidateLower: 100, CandidateUpper: 200},
			{ScenarioID: 1, BaselineLower: 900, BaselineUpper: 1500, CandidateLower: 50, CandidateUpper: 150},
		},
	}
}

// Every identity field must be set: a zero field is an UNSET field, and a key
// with unset parts would match the registry loosely.
func TestProofRequiresCompleteIdentity(t *testing.T) {
	mutations := []func(*Proof){
		func(p *Proof) { p.TenantID = 0 },
		func(p *Proof) { p.AccountID = 0 },
		func(p *Proof) { p.CallID = 0 },
		func(p *Proof) { p.Transform.EndpointID = 0 },
		func(p *Proof) { p.Transform.ModelSnapshotID = 0 },
		func(p *Proof) { p.Transform.TokenizerID = 0 },
		func(p *Proof) { p.Transform.PricingTableID = 0 },
		func(p *Proof) { p.Transform.ContractVersions = 0 },
		func(p *Proof) { p.Transform.TransformID = 0 },
		func(p *Proof) { p.Transform.TransformVersion = 0 },
		func(p *Proof) { p.Transform.ScenarioSetID = 0 },
		func(p *Proof) { p.Transform.Provider = 0 },
	}
	for i, mutate := range mutations {
		p := cheapProof()
		mutate(p)
		got := ProofEvaluate(p)
		if got.Decision != Indeterminate || got.Reason != ReasonInvalidIdentity {
			t.Errorf("mutation %d: got %v/%v, want Indeterminate/invalid_identity",
				i, got.Decision, got.Reason)
		}
	}
	if got := ProofEvaluate(nil); got.Decision != Indeterminate || got.Reason != ReasonInvalidArgument {
		t.Errorf("nil proof: got %v/%v", got.Decision, got.Reason)
	}
}

// Coverage must be EXHAUSTIVE and exact: as many scenarios as coverage bits,
// each id once, and the ids must be precisely those bits. A transform proven
// cheaper in three of four cases is not proven cheaper.
func TestProofRequiresExhaustiveCoverage(t *testing.T) {
	// Fewer scenarios than coverage bits.
	p := cheapProof()
	p.Scenarios = p.Scenarios[:1]
	if got := ProofEvaluate(p); got.Reason != ReasonInvalidScenarioCoverage {
		t.Errorf("short coverage: %v", got.Reason)
	}

	// Duplicate scenario id.
	p = cheapProof()
	p.Scenarios[1].ScenarioID = 0
	if got := ProofEvaluate(p); got.Reason != ReasonInvalidScenarioCoverage {
		t.Errorf("duplicate id: %v", got.Reason)
	}

	// Right count, wrong ids: covers bits {0,2} while coverage names {0,1}.
	p = cheapProof()
	p.Scenarios[1].ScenarioID = 2
	if got := ProofEvaluate(p); got.Reason != ReasonInvalidScenarioCoverage {
		t.Errorf("mismatched ids: %v", got.Reason)
	}

	// An id outside the scenario space.
	p = cheapProof()
	p.Scenarios[1].ScenarioID = MaxScenarios
	if got := ProofEvaluate(p); got.Reason != ReasonInvalidScenarioCoverage {
		t.Errorf("out-of-range id: %v", got.Reason)
	}

	// No scenarios at all.
	p = cheapProof()
	p.Scenarios = nil
	if got := ProofEvaluate(p); got.Reason != ReasonInvalidScenarioCount {
		t.Errorf("empty scenario set: %v", got.Reason)
	}
}

// The cost test is STRICT and worst-case: candidate UPPER + margin must be below
// baseline LOWER. Equality is not cheaper.
func TestProofCostIsStrictAndWorstCase(t *testing.T) {
	if got := ProofCostEvaluate(cheapProof()); got.Verdict != CostProven {
		t.Fatalf("a clearly cheaper proof should prove: %v/%v", got.Verdict, got.Reason)
	}

	// Equality is rejected, not accepted.
	p := cheapProof()
	p.Scenarios[0].CandidateUpper = p.Scenarios[0].BaselineLower
	if got := ProofCostEvaluate(p); got.Verdict != CostRejected || got.Reason != ReasonNotStrictlyCheaper {
		t.Errorf("equal cost must be rejected: %v/%v", got.Verdict, got.Reason)
	}

	// The safety margin must be cleared too.
	p = cheapProof()
	p.SafetyMargin = 10000
	if got := ProofCostEvaluate(p); got.Verdict != CostRejected {
		t.Errorf("a margin that swallows the saving must reject: %v", got.Verdict)
	}

	// One losing scenario sinks the whole proof, however good the others are.
	p = cheapProof()
	p.Scenarios[1].CandidateUpper = 99999
	if got := ProofCostEvaluate(p); got.Verdict != CostRejected {
		t.Errorf("one losing scenario must sink the proof: %v", got.Verdict)
	}
}

// Inverted or negative bounds are INDETERMINATE, never silently reordered.
func TestProofRejectsBadMoneyBounds(t *testing.T) {
	for i, mutate := range []func(*Proof){
		func(p *Proof) { p.Scenarios[0].BaselineLower = -1 },
		func(p *Proof) { p.Scenarios[0].CandidateUpper = -1 },
		func(p *Proof) { p.Scenarios[0].BaselineLower = 2000; p.Scenarios[0].BaselineUpper = 1000 },
		func(p *Proof) { p.Scenarios[0].CandidateLower = 500; p.Scenarios[0].CandidateUpper = 100 },
	} {
		p := cheapProof()
		mutate(p)
		if got := ProofCostEvaluate(p); got.Verdict != CostIndeterminate ||
			got.Reason != ReasonInvalidMoneyBound {
			t.Errorf("bad bound %d: %v/%v", i, got.Verdict, got.Reason)
		}
	}
	p := cheapProof()
	p.SafetyMargin = -1
	if got := ProofCostEvaluate(p); got.Reason != ReasonInvalidMoneyBound {
		t.Errorf("negative margin: %v", got.Reason)
	}
}

// Overflow is INDETERMINATE, never a wrapped comparison that could authorize a
// transform on nonsense arithmetic.
func TestProofMoneyOverflowIsIndeterminate(t *testing.T) {
	p := cheapProof()
	p.Scenarios[0].CandidateUpper = math.MaxInt64
	p.Scenarios[0].BaselineUpper = math.MaxInt64
	p.SafetyMargin = math.MaxInt64
	got := ProofCostEvaluate(p)
	if got.Verdict != CostIndeterminate || got.Reason != ReasonMoneyOverflow {
		t.Errorf("overflow must be indeterminate: %v/%v", got.Verdict, got.Reason)
	}
}

// THE SAFETY PROPERTY OF THE INITIAL RELEASE: the production registry is empty,
// so no transform can be authorized. If this ever fails, a transform became
// authorizable — and registrySignatureValid() is still a stub, so that must not
// happen without real artifact verification landing first.
func TestProofRegistryIsEmptySoNothingIsAuthorized(t *testing.T) {
	if RegistryEntryCount() != 0 {
		t.Fatal("the production registry gained an entry while signature " +
			"verification is still a stub — implement it before adding transforms")
	}
	got := ProofEvaluate(cheapProof())
	if got.Decision == Intervene {
		t.Fatal("a transform was authorized against an empty registry")
	}
	if got.Decision != PassThrough || got.Reason != ReasonRegistryAbsent {
		t.Errorf("got %v/%v, want PassThrough/registry_absent", got.Decision, got.Reason)
	}
}

// A stale generation is a definite "run the original", distinct from "cannot
// tell".
func TestProofStaleGenerationPassesThrough(t *testing.T) {
	p := cheapProof()
	p.RegistryGeneration = RegistryGeneration() + 1
	got := ProofEvaluate(p)
	if got.Decision != PassThrough || got.Reason != ReasonRegistryStale {
		t.Errorf("got %v/%v, want PassThrough/registry_stale", got.Decision, got.Reason)
	}
}

// Cost evidence alone can never authorize: the helper has no Intervene value.
func TestProofCostEvidenceCannotAuthorize(t *testing.T) {
	got := ProofCostEvaluate(cheapProof())
	if got.Verdict != CostProven {
		t.Fatal("expected a proven cost")
	}
	// Yet the full evaluation still refuses, because the registry gates it.
	if full := ProofEvaluate(cheapProof()); full.Decision == Intervene {
		t.Error("proven cost must not by itself authorize a transform")
	}
}

func TestProofDecisionAndReasonStrings(t *testing.T) {
	if PassThrough.String() != "pass_through" || Intervene.String() != "intervene" ||
		Indeterminate.String() != "indeterminate" {
		t.Error("decision strings drifted")
	}
	if ReasonNotStrictlyCheaper.String() != "not_strictly_cheaper" ||
		ReasonProofAccepted.String() != "proof_accepted" ||
		ReasonRegistryAbsent.String() != "registry_absent" {
		t.Error("reason strings drifted")
	}
}
