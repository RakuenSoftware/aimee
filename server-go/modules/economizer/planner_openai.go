package economizer

import "math"

// Local-only GPT-5.6 proof planner.
//
// Ported from src/modules/economizer/economizer_openai.c.
//
// The planner NEVER calls OpenAI and NEVER predicts cache residency. It produces
// non-authorizing local accounting evidence only: a cost verdict and a scenario,
// never a dispatch decision. Everything it cannot prove locally becomes a
// refusal reason rather than an assumption.

const (
	// OpenAIGPT56LongContextThreshold is the token count above which long-context
	// pricing applies.
	OpenAIGPT56LongContextThreshold = 272000
	// OpenAIGPT56ScenarioCount is the scenario set size this planner emits.
	OpenAIGPT56ScenarioCount = 1
)

// OpenAIEndpoint identifies the wire the request targets.
type OpenAIEndpoint uint32

const (
	OpenAIResponses       OpenAIEndpoint = 1
	OpenAIChatCompletions OpenAIEndpoint = 2
)

// OpenAIPrices are per-token rates in fixed-point money.
type OpenAIPrices struct {
	InputPerToken      Money
	CachedReadPerToken Money
	CacheWritePerToken Money
	OutputPerToken     Money
}

// OpenAIContext is the pinned identity and pricing the planner works against.
type OpenAIContext struct {
	PinnedModel          string
	ModelSnapshotID      uint64
	TokenizerID          uint64
	PricingTableID       uint64
	ContractVersions     uint64
	TokenizerGuardTokens uint64
	SafetyMargin         Money
	Prices               OpenAIPrices
}

// TokenEvidence is a token count plus proof of where it came from.
//
// The binding fields exist so a count cannot be reused across a different
// request, model or tokenizer: evidence is only valid for the exact serialized
// bytes it was measured over.
type TokenEvidence struct {
	Provider        Provider
	EndpointID      uint32
	ModelSnapshotID uint64
	TokenizerID     uint64
	SerializedSize  int
	InputTokens     uint64
	Source          TokenSource
}

// OpenAIPlanInput is one baseline/candidate pair to cost.
type OpenAIPlanInput struct {
	BaselineJSON    string
	CandidateJSON   string
	Context         *OpenAIContext
	BaselineTokens  *TokenEvidence
	CandidateTokens *TokenEvidence
	Endpoint        OpenAIEndpoint
}

// OpenAIPlan is the planner's non-authorizing output.
type OpenAIPlan struct {
	CostVerdict          CostVerdict
	Reason               Reason
	BaselineBreakpoints  int
	CandidateBreakpoints int
	Scenario             Scenario
}

func openAIPlanResult(r Reason) OpenAIPlan {
	return OpenAIPlan{CostVerdict: CostIndeterminate, Reason: r}
}

// mulMoney multiplies with overflow detection.
func mulMoney(tokens uint64, rate Money) (Money, bool) {
	if rate < 0 {
		return 0, false
	}
	if rate != 0 && tokens > uint64(math.MaxInt64)/uint64(rate) {
		return 0, false
	}
	return Money(tokens) * rate, true
}

// evidenceReason validates that a token count actually describes THIS request.
//
// Any mismatch, or a non-local count, is a refusal. A remote estimate is refused
// because it is not exact; a remote exact count with no published price is
// refused because obtaining it may add unknown cost to the candidate path — so
// exact tokens alone are not sufficient.
func evidenceReason(e *TokenEvidence, ctx *OpenAIContext, json string, endpoint OpenAIEndpoint) Reason {
	if e == nil || ctx == nil ||
		e.Provider != ProviderOpenAI || e.EndpointID != uint32(endpoint) ||
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

// pricesValid enforces the published GPT-5.6 rate RELATIONSHIPS, not merely
// positivity.
//
// Cached reads are a tenth of input, cache writes are 1.25x input, and output is
// even. If a pricing table drifts from those ratios it is no longer the table
// this planner was proven against, so costing must refuse rather than compute
// with numbers it does not recognise.
func pricesValid(p OpenAIPrices) bool {
	if p.InputPerToken <= 0 || p.CachedReadPerToken <= 0 ||
		p.CacheWritePerToken <= 0 || p.OutputPerToken <= 0 {
		return false
	}
	if p.InputPerToken%10 != 0 || p.CachedReadPerToken != p.InputPerToken/10 {
		return false
	}
	if p.InputPerToken%4 != 0 || p.InputPerToken/4 > math.MaxInt64/5 {
		return false
	}
	if p.CacheWritePerToken != (p.InputPerToken/4)*5 {
		return false
	}
	return p.OutputPerToken%2 == 0
}

// withinGuard reports whether a token count sits inside the guard band around
// the long-context boundary.
//
// Near the threshold a small tokenizer disagreement flips the price tier, so the
// planner refuses instead of betting on which side it lands.
func withinGuard(tokens, guard uint64) bool {
	const boundary = OpenAIGPT56LongContextThreshold
	if tokens >= boundary {
		return tokens-boundary <= guard
	}
	return boundary-tokens <= guard
}

// inputCost prices input tokens, DOUBLING above the long-context threshold.
func inputCost(tokens uint64, rate Money) (Money, bool) {
	base, ok := mulMoney(tokens, rate)
	if !ok {
		return 0, false
	}
	if tokens > OpenAIGPT56LongContextThreshold {
		return moneyAdd(base, base)
	}
	return base, true
}

// outputCost prices output tokens at 1.5x under long-context.
func outputCost(tokens uint64, rate Money, longContext bool) (Money, bool) {
	if longContext {
		if rate%2 != 0 {
			return 0, false
		}
		var ok bool
		rate, ok = moneyAdd(rate, rate/2)
		if !ok {
			return 0, false
		}
	}
	return mulMoney(tokens, rate)
}

// openAICacheLayout is the cache-relevant shape of a request.
type openAICacheLayout struct {
	explicitMode    bool
	ttlPresent      bool
	cacheKeyPresent bool
	cacheKey        string
	breakpoints     int
	maxOutputTokens uint64
}

// parseOpenAILayout extracts the cache layout, refusing anything it cannot read
// exactly.
func parseOpenAILayout(jsonText, pinnedModel string, endpoint OpenAIEndpoint) (openAICacheLayout, bool) {
	var out openAICacheLayout
	root := ParseJSON(jsonText)
	if root == nil || root.Kind != JSONObject {
		return out, false
	}
	// The model must be the PINNED one: a proof costed against one model says
	// nothing about another.
	if root.GetString("model") != pinnedModel {
		return out, false
	}
	if err := rejectDuplicateKeys(root); err != nil {
		return out, false
	}

	// Explicit cache mode with no breakpoints is the only supported layout; the
	// provider contract documents that caching and cache-write charges are
	// disabled for it, which is what makes local costing sound.
	if cc := root.Get("prompt_cache"); cc != nil && cc.Kind == JSONObject {
		out.explicitMode = cc.GetString("mode") == "explicit"
		out.ttlPresent = cc.Get("ttl") != nil
		if k := cc.Get("key"); k != nil && k.Kind == JSONString {
			out.cacheKeyPresent = true
			out.cacheKey = k.Str
		}
	}
	out.breakpoints = countBreakpoints(root)

	switch endpoint {
	case OpenAIResponses:
		if v := root.Get("max_output_tokens"); v != nil && v.Kind == JSONNumber {
			out.maxOutputTokens = uint64(v.Num)
		}
	case OpenAIChatCompletions:
		if v := root.Get("max_completion_tokens"); v != nil && v.Kind == JSONNumber {
			out.maxOutputTokens = uint64(v.Num)
		} else if v := root.Get("max_tokens"); v != nil && v.Kind == JSONNumber {
			out.maxOutputTokens = uint64(v.Num)
		}
	}
	return out, true
}

// countBreakpoints counts cache_control markers anywhere in the document.
func countBreakpoints(v *JSONValue) int {
	if v == nil {
		return 0
	}
	n := 0
	switch v.Kind {
	case JSONObject:
		for i, k := range v.Keys {
			if k == "cache_control" {
				n++
			}
			n += countBreakpoints(v.Vals[i])
		}
	case JSONArray:
		for _, item := range v.Items {
			n += countBreakpoints(item)
		}
	}
	return n
}

// rejectDuplicateKeys refuses a document with repeated object names anywhere.
//
// A duplicate name means two parsers can disagree about the request, so costing
// one reading proves nothing about what the provider will execute.
func rejectDuplicateKeys(v *JSONValue) error {
	if v == nil {
		return nil
	}
	switch v.Kind {
	case JSONObject:
		seen := make(map[string]bool, len(v.Keys))
		for i, k := range v.Keys {
			if seen[k] {
				return errDuplicateKey
			}
			seen[k] = true
			if err := rejectDuplicateKeys(v.Vals[i]); err != nil {
				return err
			}
		}
	case JSONArray:
		for _, item := range v.Items {
			if err := rejectDuplicateKeys(item); err != nil {
				return err
			}
		}
	}
	return nil
}

var errDuplicateKey = &plannerError{"duplicate key"}

type plannerError struct{ msg string }

func (e *plannerError) Error() string { return e.msg }

// OpenAIGPT56Plan produces local accounting evidence for one baseline/candidate
// pair.
//
// Returns a cost verdict and a scenario — never an authorization. Only
// ProofEvaluate can authorize, and it requires a signed registry entry this
// planner does not and cannot supply.
func OpenAIGPT56Plan(in *OpenAIPlanInput) OpenAIPlan {
	if in == nil || in.Context == nil || in.BaselineJSON == "" || in.CandidateJSON == "" {
		return openAIPlanResult(ReasonInvalidArgument)
	}
	ctx := in.Context
	if ctx.PinnedModel == "" || ctx.ModelSnapshotID == 0 || ctx.TokenizerID == 0 ||
		ctx.PricingTableID == 0 || ctx.ContractVersions == 0 {
		return openAIPlanResult(ReasonInvalidIdentity)
	}
	if in.Endpoint != OpenAIResponses && in.Endpoint != OpenAIChatCompletions {
		return openAIPlanResult(ReasonUnsupportedEndpoint)
	}
	if r := evidenceReason(in.BaselineTokens, ctx, in.BaselineJSON, in.Endpoint); r != ReasonNone {
		return openAIPlanResult(r)
	}
	if r := evidenceReason(in.CandidateTokens, ctx, in.CandidateJSON, in.Endpoint); r != ReasonNone {
		return openAIPlanResult(r)
	}
	if !pricesValid(ctx.Prices) || ctx.SafetyMargin < 0 {
		return openAIPlanResult(ReasonPricingUnavailable)
	}

	baseline, ok1 := parseOpenAILayout(in.BaselineJSON, ctx.PinnedModel, in.Endpoint)
	candidate, ok2 := parseOpenAILayout(in.CandidateJSON, ctx.PinnedModel, in.Endpoint)
	if !ok1 || !ok2 {
		return openAIPlanResult(ReasonInvalidRequestShape)
	}

	out := openAIPlanResult(ReasonNone)
	out.BaselineBreakpoints = baseline.breakpoints
	out.CandidateBreakpoints = candidate.breakpoints

	if !baseline.explicitMode || !candidate.explicitMode {
		out.Reason = ReasonUnsupportedCacheLayout
		return out
	}
	// The two paths must share a cache identity, or their costs are not
	// comparable.
	if baseline.ttlPresent != candidate.ttlPresent ||
		baseline.cacheKeyPresent != candidate.cacheKeyPresent ||
		(baseline.cacheKeyPresent && baseline.cacheKey != candidate.cacheKey) {
		out.Reason = ReasonUnsupportedCacheLayout
		return out
	}
	if baseline.maxOutputTokens != candidate.maxOutputTokens {
		out.Reason = ReasonOutputBoundUnavailable
		return out
	}
	if baseline.breakpoints != candidate.breakpoints {
		out.Reason = ReasonUnsupportedCacheLayout
		return out
	}
	// Breakpoints are parsed but still DENIED: proving a protected prefix needs
	// exact serialized ranges, which are not implemented, so a request carrying
	// them cannot be costed soundly.
	if baseline.breakpoints != 0 {
		out.Reason = ReasonProtectedPrefixUnproven
		return out
	}

	baselineTokens := in.BaselineTokens.InputTokens
	candidateTokens := in.CandidateTokens.InputTokens
	if withinGuard(baselineTokens, ctx.TokenizerGuardTokens) ||
		withinGuard(candidateTokens, ctx.TokenizerGuardTokens) {
		out.Reason = ReasonTokenGuardBand
		return out
	}

	baselineInput, ok := inputCost(baselineTokens, ctx.Prices.InputPerToken)
	if !ok {
		out.Reason = ReasonMoneyOverflow
		return out
	}
	candidateInput, ok := inputCost(candidateTokens, ctx.Prices.InputPerToken)
	if !ok {
		out.Reason = ReasonMoneyOverflow
		return out
	}
	candidateOutput, ok := outputCost(candidate.maxOutputTokens, ctx.Prices.OutputPerToken,
		candidateTokens > OpenAIGPT56LongContextThreshold)
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

	// Cost the scenario through the shared proof helper, so the planner and the
	// authorizer agree on what "strictly cheaper" means.
	local := &Proof{
		TenantID: 1, AccountID: 1, CallID: 1,
		Transform: RegistryKey{
			Provider: ProviderOpenAI, EndpointID: uint32(in.Endpoint),
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
