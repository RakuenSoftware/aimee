package economizer

import (
	"math"
	"math/bits"
)

// The economizer proof planner.
//
// Ported from src/modules/economizer/economizer_proof.c.
//
// This decides whether a request transform may be applied at all. Its whole
// design is fail-closed: a transform is authorized ONLY when it is provably,
// strictly cheaper across an exhaustive scenario set, against a signed registry,
// with overflow-safe money arithmetic. Anything unproven is INDETERMINATE rather
// than allowed — an economizer that guesses wrong costs real money silently.

// Money is a fixed-point currency amount; negative values are invalid input.
type Money = int64

// Decision is the authorization outcome.
type Decision int

const (
	PassThrough Decision = iota
	Intervene
	Indeterminate
)

func (d Decision) String() string {
	switch d {
	case PassThrough:
		return "pass_through"
	case Intervene:
		return "intervene"
	case Indeterminate:
		return "indeterminate"
	}
	return "unknown"
}

// Reason records WHY a decision came out as it did, so a refusal is auditable
// rather than an opaque "no".
type Reason int

const (
	ReasonNone Reason = iota
	ReasonInvalidArgument
	ReasonInvalidIdentity
	ReasonInvalidScenarioCount
	ReasonInvalidScenarioCoverage
	ReasonInvalidMoneyBound
	ReasonMoneyOverflow
	ReasonRegistryUnverified
	ReasonRegistryStale
	ReasonRegistryAbsent
	ReasonNotStrictlyCheaper
	ReasonUnsupportedEndpoint
	ReasonModelNotPinned
	ReasonTokenizerNotLocalExact
	ReasonRemoteTokenCount
	ReasonRemoteTokenCountUnpriced
	ReasonInvalidRequestShape
	ReasonUnsupportedCacheLayout
	ReasonInvalidCacheControl
	ReasonProtectedPrefixUnproven
	ReasonTokenGuardBand
	ReasonOutputBoundUnavailable
	ReasonPricingUnavailable
	ReasonProofAccepted
)

var reasonNames = map[Reason]string{
	ReasonNone: "none", ReasonInvalidArgument: "invalid_argument",
	ReasonInvalidIdentity: "invalid_identity", ReasonInvalidScenarioCount: "invalid_scenario_count",
	ReasonInvalidScenarioCoverage: "invalid_scenario_coverage",
	ReasonInvalidMoneyBound:       "invalid_money_bound", ReasonMoneyOverflow: "money_overflow",
	ReasonRegistryUnverified: "registry_unverified", ReasonRegistryStale: "registry_stale",
	ReasonRegistryAbsent: "registry_absent", ReasonNotStrictlyCheaper: "not_strictly_cheaper",
	ReasonUnsupportedEndpoint: "unsupported_endpoint", ReasonModelNotPinned: "model_not_pinned",
	ReasonTokenizerNotLocalExact:   "tokenizer_not_local_exact",
	ReasonRemoteTokenCount:         "remote_token_count",
	ReasonRemoteTokenCountUnpriced: "remote_token_count_unpriced",
	ReasonInvalidRequestShape:      "invalid_request_shape",
	ReasonUnsupportedCacheLayout:   "unsupported_cache_layout",
	ReasonInvalidCacheControl:      "invalid_cache_control",
	ReasonProtectedPrefixUnproven:  "protected_prefix_unproven",
	ReasonTokenGuardBand:           "token_guard_band",
	ReasonOutputBoundUnavailable:   "output_bound_unavailable",
	ReasonPricingUnavailable:       "pricing_unavailable",
	ReasonProofAccepted:            "proof_accepted",
}

func (r Reason) String() string {
	if s, ok := reasonNames[r]; ok {
		return s
	}
	return "unknown"
}

// Provider identifies the upstream a proof targets.
type Provider int

const (
	ProviderOpenAI    Provider = 1
	ProviderAnthropic Provider = 2
)

// TokenSource records how a token count was obtained.
type TokenSource int

const (
	TokenSourceNone TokenSource = iota
	TokenSourceLocalExact
	TokenSourceRemoteEstimate
	// TokenSourceRemoteExactUnpriced: the provider says the count is exact but
	// publishes no finite monetary bound for the counting call itself. Exact
	// tokens are NOT sufficient to authorize a transform when obtaining them may
	// add unknown cost to the candidate path.
	TokenSourceRemoteExactUnpriced
)

// MaxScenarios bounds a proof's scenario set.
const MaxScenarios = 16

// RegistryKey is the signed identity of a transform.
type RegistryKey struct {
	Provider         Provider
	EndpointID       uint32
	ModelSnapshotID  uint64
	TokenizerID      uint64
	PricingTableID   uint64
	ContractVersions uint64
	TransformID      uint64
	TransformVersion uint64
	ScenarioSetID    uint64
	// ScenarioCoverage names the EXHAUSTIVE scenario set a proof must carry
	// exactly once. Partial coverage is the failure mode this exists to stop: a
	// transform proven cheaper in three of four cases is not proven cheaper.
	ScenarioCoverage uint64
}

// Scenario is one costed case: bounds for the baseline and candidate paths.
type Scenario struct {
	ScenarioID     uint32
	BaselineLower  Money
	BaselineUpper  Money
	CandidateLower Money
	CandidateUpper Money
}

// Proof is a complete-call authorization request.
type Proof struct {
	TenantID           uint64
	AccountID          uint64
	CallID             uint64
	RegistryGeneration uint64
	Transform          RegistryKey
	Scenarios          []Scenario
	SafetyMargin       Money
}

// ProofResult is the authorization outcome plus its reason.
type ProofResult struct {
	Decision Decision
	Reason   Reason
}

// CostVerdict is cost EVIDENCE, deliberately distinct from authorization: this
// helper has no Intervene value, so cost evidence alone can never authorize.
type CostVerdict int

const (
	CostRejected CostVerdict = iota
	CostProven
	CostIndeterminate
)

// CostResult is a cost verdict plus its reason.
type CostResult struct {
	Verdict CostVerdict
	Reason  Reason
}

// keyValid requires every identity field to be set. A zero field is an UNSET
// field, and a key with unset parts would match loosely against the registry.
func (k RegistryKey) valid() bool {
	return (k.Provider == ProviderOpenAI || k.Provider == ProviderAnthropic) &&
		k.EndpointID != 0 && k.ModelSnapshotID != 0 && k.TokenizerID != 0 &&
		k.PricingTableID != 0 && k.ContractVersions != 0 && k.TransformID != 0 &&
		k.TransformVersion != 0 && k.ScenarioSetID != 0 && k.ScenarioCoverage != 0
}

// moneyAdd adds with overflow detection. Overflow is INDETERMINATE, never a
// wrapped comparison that could authorize a transform on nonsense arithmetic.
func moneyAdd(a, b Money) (Money, bool) {
	if b > 0 && a > math.MaxInt64-b {
		return 0, false
	}
	if b < 0 && a < math.MinInt64-b {
		return 0, false
	}
	return a + b, true
}

// validateProof checks identity and scenario coverage.
func validateProof(p *Proof) ProofResult {
	if p == nil {
		return ProofResult{Indeterminate, ReasonInvalidArgument}
	}
	if p.TenantID == 0 || p.AccountID == 0 || p.CallID == 0 || !p.Transform.valid() {
		return ProofResult{Indeterminate, ReasonInvalidIdentity}
	}
	if len(p.Scenarios) == 0 || len(p.Scenarios) > MaxScenarios {
		return ProofResult{Indeterminate, ReasonInvalidScenarioCount}
	}
	// The coverage mask must have exactly as many bits as there are scenarios,
	// and the scenarios must be exactly those bits, each once. This is what makes
	// the set EXHAUSTIVE rather than merely non-empty.
	if bits.OnesCount64(p.Transform.ScenarioCoverage) != len(p.Scenarios) {
		return ProofResult{Indeterminate, ReasonInvalidScenarioCoverage}
	}
	var seen uint64
	for _, s := range p.Scenarios {
		if s.ScenarioID >= MaxScenarios || seen&(uint64(1)<<s.ScenarioID) != 0 {
			return ProofResult{Indeterminate, ReasonInvalidScenarioCoverage}
		}
		seen |= uint64(1) << s.ScenarioID
	}
	if seen != p.Transform.ScenarioCoverage {
		return ProofResult{Indeterminate, ReasonInvalidScenarioCoverage}
	}
	if p.SafetyMargin < 0 {
		return ProofResult{Indeterminate, ReasonInvalidMoneyBound}
	}
	return ProofResult{Intervene, ReasonProofAccepted}
}

// ProofCostEvaluate checks the cost bounds without authorizing anything.
//
// The test is STRICT and worst-case: the candidate's UPPER bound plus the safety
// margin must be below the baseline's LOWER bound — per scenario and globally.
// Comparing averages, or allowing equality, would let a transform that is
// sometimes more expensive through.
func ProofCostEvaluate(p *Proof) CostResult {
	if v := validateProof(p); v.Decision != Intervene {
		return CostResult{CostIndeterminate, v.Reason}
	}

	globalBaselineLower := Money(math.MaxInt64)
	globalCandidateUpper := Money(0)
	for _, s := range p.Scenarios {
		if s.BaselineLower < 0 || s.BaselineUpper < 0 ||
			s.CandidateLower < 0 || s.CandidateUpper < 0 ||
			s.BaselineLower > s.BaselineUpper || s.CandidateLower > s.CandidateUpper {
			return CostResult{CostIndeterminate, ReasonInvalidMoneyBound}
		}
		withMargin, ok := moneyAdd(s.CandidateUpper, p.SafetyMargin)
		if !ok {
			return CostResult{CostIndeterminate, ReasonMoneyOverflow}
		}
		if withMargin >= s.BaselineLower {
			return CostResult{CostRejected, ReasonNotStrictlyCheaper}
		}
		if s.BaselineLower < globalBaselineLower {
			globalBaselineLower = s.BaselineLower
		}
		if s.CandidateUpper > globalCandidateUpper {
			globalCandidateUpper = s.CandidateUpper
		}
	}

	// The global check is not redundant: per-scenario wins do not imply the worst
	// candidate beats the best baseline across the whole set.
	globalWithMargin, ok := moneyAdd(globalCandidateUpper, p.SafetyMargin)
	if !ok {
		return CostResult{CostIndeterminate, ReasonMoneyOverflow}
	}
	if globalWithMargin >= globalBaselineLower {
		return CostResult{CostRejected, ReasonNotStrictlyCheaper}
	}
	return CostResult{CostProven, ReasonProofAccepted}
}

// Registry is the immutable, signed transform registry.
type Registry struct {
	Generation uint64
	Keys       []RegistryKey
}

// productionRegistry mirrors the C: signature-verified and EMPTY in the initial
// release, so ProofEvaluate cannot return Intervene yet. That emptiness is the
// safety property, not an oversight — nothing is authorized until a transform is
// signed into the registry.
var productionRegistry = Registry{Generation: 1}

// registrySignatureValid reports whether the production registry's artifact
// verifies.
//
// PORTING NOTE: the C verifies a detached Ed25519 signature over a canonical
// manifest, with the key and signature compiled in. This port keeps the CHECK in
// the decision path — an unverified registry is INDETERMINATE — but the signed
// artifact itself has not been carried over, so this returns true for the empty
// production registry. It MUST be replaced with real verification before any
// transform is added to the registry, and there is a test asserting the registry
// is still empty so this cannot silently start authorizing.
func registrySignatureValid() bool {
	return true
}

// RegistryGeneration is the production registry's generation.
func RegistryGeneration() uint64 { return productionRegistry.Generation }

// RegistryEntryCount is the number of registered transforms.
func RegistryEntryCount() int { return len(productionRegistry.Keys) }

func registryContains(r Registry, k RegistryKey) bool {
	if !k.valid() {
		return false
	}
	for _, e := range r.Keys {
		if e == k {
			return true
		}
	}
	return false
}

// ProofEvaluate evaluates one complete-call proof against the production
// registry.
//
// The ordering is the safety design: identity, then registry verification, then
// generation, then membership, and only then cost. A stale generation or an
// absent key is PASS_THROUGH (a definite "no, run the original"), whereas an
// unverifiable registry is INDETERMINATE (we cannot even say no safely).
func ProofEvaluate(p *Proof) ProofResult {
	valid := validateProof(p)
	if valid.Decision != Intervene {
		return valid
	}
	if !registrySignatureValid() {
		return ProofResult{Indeterminate, ReasonRegistryUnverified}
	}
	if p.RegistryGeneration != productionRegistry.Generation {
		return ProofResult{PassThrough, ReasonRegistryStale}
	}
	if !registryContains(productionRegistry, p.Transform) {
		return ProofResult{PassThrough, ReasonRegistryAbsent}
	}
	cost := ProofCostEvaluate(p)
	switch cost.Verdict {
	case CostProven:
		return ProofResult{Intervene, cost.Reason}
	case CostRejected:
		return ProofResult{PassThrough, cost.Reason}
	}
	return ProofResult{Indeterminate, cost.Reason}
}
