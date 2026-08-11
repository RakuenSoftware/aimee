package economizer

import "math"

// Local-only Anthropic proof planner.
//
// Ported from src/modules/economizer/economizer_anthropic.c.
//
// Like the OpenAI planner: no provider calls, no cache-residency prediction,
// non-authorizing evidence only.

// AnthropicPrices are per-token rates, including the optional long-context tier.
type AnthropicPrices struct {
	InputPerToken        Money
	CachedReadPerToken   Money
	CacheWrite5mPerToken Money
	CacheWrite1hPerToken Money
	OutputPerToken       Money
	LongInputPerToken    Money
	LongOutputPerToken   Money
	// LongContextThreshold of 0 means the model publishes no long-context tier.
	LongContextThreshold uint64
}

// AnthropicContext is the pinned identity and pricing for a plan.
type AnthropicContext struct {
	PinnedModel      string
	ModelSnapshotID  uint64
	TokenizerID      uint64
	PricingTableID   uint64
	ContractVersions uint64
	// HasBetaHeaders disqualifies the request outright: a beta header can change
	// billing or caching semantics the pinned contract does not describe, so
	// nothing costed against that contract would be sound.
	HasBetaHeaders bool
	SafetyMargin   Money
	Prices         AnthropicPrices
}

// AnthropicPlanInput is one baseline/candidate pair to cost.
type AnthropicPlanInput struct {
	BaselineJSON    string
	CandidateJSON   string
	Context         *AnthropicContext
	BaselineTokens  *TokenEvidence
	CandidateTokens *TokenEvidence
}

// AnthropicPlan is the planner's non-authorizing output.
type AnthropicPlan struct {
	CostVerdict            CostVerdict
	Reason                 Reason
	BaselineBreakpoints5m  int
	BaselineBreakpoints1h  int
	CandidateBreakpoints5m int
	CandidateBreakpoints1h int
	Scenario               Scenario
}

func anthropicPlanResult(r Reason) AnthropicPlan {
	return AnthropicPlan{CostVerdict: CostIndeterminate, Reason: r}
}

// anthropicLayout is the cache-relevant shape of a request.
type anthropicLayout struct {
	markers5m       int
	markers1h       int
	saw5m           bool
	automaticCache  bool
	hasTools        bool
	maxOutputTokens uint64
}

// recordControl validates one cache_control marker.
//
// Only "ephemeral" is understood. An absent ttl means 5m. The ORDERING RULE is
// the subtle one: a 1h marker may not follow a 5m marker, because Anthropic
// nests cache breakpoints by lifetime and a shorter-lived prefix cannot contain
// a longer-lived one. A request that violates it would not cache the way a
// naive cost model assumes, so it is refused rather than mispriced.
func recordControl(control *JSONValue, layout *anthropicLayout) bool {
	if control == nil || control.Kind != JSONObject {
		return false
	}
	if control.GetString("type") != "ephemeral" {
		return false
	}
	ttl := control.Get("ttl")
	if ttl == nil || (ttl.Kind == JSONString && ttl.Str == "5m") {
		layout.markers5m++
		layout.saw5m = true
		return true
	}
	if ttl.Kind != JSONString || ttl.Str != "1h" || layout.saw5m {
		return false
	}
	layout.markers1h++
	return true
}

// scanControls walks the document recording every cache_control marker.
func scanControls(node *JSONValue, layout *anthropicLayout) bool {
	if node == nil {
		return false
	}
	switch node.Kind {
	case JSONObject:
		for i, key := range node.Keys {
			if key == "cache_control" {
				if !recordControl(node.Vals[i], layout) {
					return false
				}
			} else if !scanControls(node.Vals[i], layout) {
				return false
			}
		}
	case JSONArray:
		for _, item := range node.Items {
			if !scanControls(item, layout) {
				return false
			}
		}
	}
	return true
}

// parseAnthropicLayout extracts the cache layout, refusing anything unreadable.
func parseAnthropicLayout(jsonText, pinnedModel string) (anthropicLayout, bool) {
	var out anthropicLayout
	root := ParseJSON(jsonText)
	if root == nil || root.Kind != JSONObject {
		return out, false
	}
	if root.GetString("model") != pinnedModel {
		return out, false
	}
	if err := rejectDuplicateKeys(root); err != nil {
		return out, false
	}
	if v := root.Get("max_tokens"); v != nil && v.Kind == JSONNumber {
		out.maxOutputTokens = uint64(v.Num)
	}
	// Tools and automatic caching both make the cached prefix depend on state
	// this planner cannot see locally.
	if t := root.Get("tools"); t != nil {
		out.hasTools = true
	}
	if c := root.Get("cache"); c != nil && c.Kind == JSONObject {
		out.automaticCache = true
	}
	if !scanControls(root, &out) {
		return out, false
	}
	return out, true
}

// anthropicEvidenceReason validates that a token count describes THIS request.
func anthropicEvidenceReason(e *TokenEvidence, ctx *AnthropicContext, json string) Reason {
	if e == nil || ctx == nil ||
		e.Provider != ProviderAnthropic ||
		e.ModelSnapshotID != ctx.ModelSnapshotID || e.TokenizerID != ctx.TokenizerID ||
		e.SerializedSize != len(json) {
		return ReasonTokenizerNotLocalExact
	}
	switch e.Source {
	case TokenSourceRemoteExactUnpriced:
		return ReasonRemoteTokenCountUnpriced
	case TokenSourceRemoteEstimate:
		return ReasonRemoteTokenCount
	case TokenSourceLocalExact:
		return ReasonNone
	}
	return ReasonTokenizerNotLocalExact
}

// anthropicPricesValid enforces the published caching contract: read is 0.1x
// input, a 5m write is 1.25x, a 1h write is 2x.
//
// As with OpenAI, a table that does not satisfy these ratios is not the contract
// the planner was proven against, so it refuses rather than computing with
// numbers it cannot vouch for.
func anthropicPricesValid(p AnthropicPrices) bool {
	if p.InputPerToken <= 0 || p.CachedReadPerToken <= 0 ||
		p.CacheWrite5mPerToken <= 0 || p.CacheWrite1hPerToken <= 0 || p.OutputPerToken <= 0 {
		return false
	}
	if p.LongContextThreshold != 0 && (p.LongInputPerToken <= 0 || p.LongOutputPerToken <= 0) {
		return false
	}
	if p.InputPerToken%10 != 0 || p.CachedReadPerToken != p.InputPerToken/10 {
		return false
	}
	if p.InputPerToken%4 != 0 || p.InputPerToken/4 > math.MaxInt64/5 {
		return false
	}
	if p.CacheWrite5mPerToken != (p.InputPerToken/4)*5 {
		return false
	}
	if p.InputPerToken > math.MaxInt64/2 || p.CacheWrite1hPerToken != p.InputPerToken*2 {
		return false
	}
	return true
}

// anthropicRequestCost prices one request, switching to the long-context tier
// above the published threshold.
func anthropicRequestCost(inputTokens, outputTokens uint64, p AnthropicPrices) (Money, Money, bool) {
	longContext := p.LongContextThreshold != 0 && inputTokens > p.LongContextThreshold
	inputRate, outputRate := p.InputPerToken, p.OutputPerToken
	if longContext {
		inputRate, outputRate = p.LongInputPerToken, p.LongOutputPerToken
	}
	in, ok1 := mulMoney(inputTokens, inputRate)
	out, ok2 := mulMoney(outputTokens, outputRate)
	if !ok1 || !ok2 {
		return 0, 0, false
	}
	return in, out, true
}

// AnthropicPlan produces local accounting evidence for one baseline/candidate
// pair. Non-authorizing: only ProofEvaluate can authorize.
func AnthropicPlanFor(in *AnthropicPlanInput) AnthropicPlan {
	if in == nil || in.Context == nil || in.BaselineJSON == "" || in.CandidateJSON == "" {
		return anthropicPlanResult(ReasonInvalidArgument)
	}
	ctx := in.Context
	if ctx.PinnedModel == "" || ctx.ModelSnapshotID == 0 || ctx.TokenizerID == 0 ||
		ctx.PricingTableID == 0 || ctx.ContractVersions == 0 {
		return anthropicPlanResult(ReasonInvalidIdentity)
	}
	if r := anthropicEvidenceReason(in.BaselineTokens, ctx, in.BaselineJSON); r != ReasonNone {
		return anthropicPlanResult(r)
	}
	if r := anthropicEvidenceReason(in.CandidateTokens, ctx, in.CandidateJSON); r != ReasonNone {
		return anthropicPlanResult(r)
	}
	// A beta header can change billing or caching semantics the pinned contract
	// does not describe.
	if ctx.HasBetaHeaders {
		return anthropicPlanResult(ReasonInvalidRequestShape)
	}
	if !anthropicPricesValid(ctx.Prices) || ctx.SafetyMargin < 0 {
		return anthropicPlanResult(ReasonPricingUnavailable)
	}

	baseline, ok1 := parseAnthropicLayout(in.BaselineJSON, ctx.PinnedModel)
	candidate, ok2 := parseAnthropicLayout(in.CandidateJSON, ctx.PinnedModel)
	if !ok1 || !ok2 {
		return anthropicPlanResult(ReasonInvalidCacheControl)
	}

	out := anthropicPlanResult(ReasonNone)
	out.BaselineBreakpoints5m = baseline.markers5m
	out.BaselineBreakpoints1h = baseline.markers1h
	out.CandidateBreakpoints5m = candidate.markers5m
	out.CandidateBreakpoints1h = candidate.markers1h

	if baseline.maxOutputTokens != candidate.maxOutputTokens {
		out.Reason = ReasonOutputBoundUnavailable
		return out
	}
	// Automatic caching and tools both make the cached prefix depend on state
	// this planner cannot observe.
	if baseline.automaticCache || candidate.automaticCache || baseline.hasTools || candidate.hasTools {
		out.Reason = ReasonUnsupportedCacheLayout
		return out
	}
	if baseline.markers5m != candidate.markers5m || baseline.markers1h != candidate.markers1h {
		out.Reason = ReasonUnsupportedCacheLayout
		return out
	}
	// Breakpoints parse but stay denied until exact protected ranges exist.
	if baseline.markers5m != 0 || baseline.markers1h != 0 {
		out.Reason = ReasonProtectedPrefixUnproven
		return out
	}

	baselineTokens := in.BaselineTokens.InputTokens
	candidateTokens := in.CandidateTokens.InputTokens
	// The baseline is costed with ZERO output: its output is not attributable to
	// the transform, so charging it to the baseline would flatter the candidate.
	baselineInput, _, ok := anthropicRequestCost(baselineTokens, 0, ctx.Prices)
	if !ok {
		out.Reason = ReasonMoneyOverflow
		return out
	}
	candidateInput, candidateOutput, ok := anthropicRequestCost(
		candidateTokens, candidate.maxOutputTokens, ctx.Prices)
	if !ok {
		out.Reason = ReasonMoneyOverflow
		return out
	}
	candidateUpper, ok := moneyAdd(candidateInput, candidateOutput)
	if !ok {
		out.Reason = ReasonMoneyOverflow
		return out
	}

	out.Scenario = Scenario{
		ScenarioID:     0,
		BaselineLower:  baselineInput,
		BaselineUpper:  baselineInput,
		CandidateLower: candidateInput,
		CandidateUpper: candidateUpper,
	}

	local := &Proof{
		TenantID: 1, AccountID: 1, CallID: 1,
		Transform: RegistryKey{
			Provider: ProviderAnthropic, EndpointID: 1,
			ModelSnapshotID: ctx.ModelSnapshotID, TokenizerID: ctx.TokenizerID,
			PricingTableID: ctx.PricingTableID, ContractVersions: ctx.ContractVersions,
			TransformID: 1, TransformVersion: 1, ScenarioSetID: 1, ScenarioCoverage: 1,
		},
		Scenarios:    []Scenario{out.Scenario},
		SafetyMargin: ctx.SafetyMargin,
	}
	cost := ProofCostEvaluate(local)
	out.CostVerdict = cost.Verdict
	out.Reason = cost.Reason
	return out
}
