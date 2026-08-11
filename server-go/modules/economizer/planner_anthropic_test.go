package economizer

import (
	"fmt"
	"testing"
)

func anthroPrices() AnthropicPrices {
	const input = 40 // divisible by 10 and 4, and 2x fits
	return AnthropicPrices{
		InputPerToken:        input,
		CachedReadPerToken:   input / 10,
		CacheWrite5mPerToken: (input / 4) * 5,
		CacheWrite1hPerToken: input * 2,
		OutputPerToken:       80,
	}
}

func anthroContext() *AnthropicContext {
	return &AnthropicContext{
		PinnedModel: "claude-x", ModelSnapshotID: 11, TokenizerID: 22,
		PricingTableID: 33, ContractVersions: 44, Prices: anthroPrices(),
	}
}

func anthroReq(maxTokens int) string {
	return fmt.Sprintf(`{"model":"claude-x","max_tokens":%d}`, maxTokens)
}

func anthroEvidence(js string, tokens uint64) *TokenEvidence {
	return &TokenEvidence{
		Provider: ProviderAnthropic, ModelSnapshotID: 11, TokenizerID: 22,
		SerializedSize: len(js), InputTokens: tokens, Source: TokenSourceLocalExact,
	}
}

func anthroInput(baseTokens, candTokens uint64) *AnthropicPlanInput {
	b, c := anthroReq(100), anthroReq(100)
	return &AnthropicPlanInput{
		BaselineJSON: b, CandidateJSON: c, Context: anthroContext(),
		BaselineTokens: anthroEvidence(b, baseTokens), CandidateTokens: anthroEvidence(c, candTokens),
	}
}

func TestAnthropicPlanProvesACheaperCandidate(t *testing.T) {
	got := AnthropicPlanFor(anthroInput(10000, 1000))
	if got.CostVerdict != CostProven || got.Reason != ReasonProofAccepted {
		t.Fatalf("got %v/%v, want CostProven/proof_accepted", got.CostVerdict, got.Reason)
	}
}

func TestAnthropicPlanRejectsNonCheaper(t *testing.T) {
	got := AnthropicPlanFor(anthroInput(1000, 10000))
	if got.CostVerdict != CostRejected || got.Reason != ReasonNotStrictlyCheaper {
		t.Errorf("got %v/%v", got.CostVerdict, got.Reason)
	}
}

// A beta header can change billing or caching semantics the pinned contract does
// not describe, so nothing costed against that contract would be sound.
func TestAnthropicPlanRefusesBetaHeaders(t *testing.T) {
	in := anthroInput(10000, 1000)
	in.Context.HasBetaHeaders = true
	if got := AnthropicPlanFor(in); got.Reason != ReasonInvalidRequestShape {
		t.Errorf("got %v, want invalid_request_shape", got.Reason)
	}
}

// THE TTL ORDERING RULE: a 1h marker may not follow a 5m marker, because cache
// breakpoints nest by lifetime and a shorter-lived prefix cannot contain a
// longer-lived one. Such a request would not cache the way a naive cost model
// assumes, so it is refused rather than mispriced.
func TestAnthropicPlanTTLOrdering(t *testing.T) {
	mk := func(js string) *AnthropicPlanInput {
		return &AnthropicPlanInput{
			BaselineJSON: js, CandidateJSON: js, Context: anthroContext(),
			BaselineTokens: anthroEvidence(js, 10000), CandidateTokens: anthroEvidence(js, 1000),
		}
	}
	// 5m then 1h -> refused.
	bad := `{"model":"claude-x","max_tokens":100,"system":[` +
		`{"cache_control":{"type":"ephemeral","ttl":"5m"}},` +
		`{"cache_control":{"type":"ephemeral","ttl":"1h"}}]}`
	if got := AnthropicPlanFor(mk(bad)); got.Reason != ReasonInvalidCacheControl {
		t.Errorf("5m-then-1h should be refused: got %v", got.Reason)
	}
	// 1h then 5m is the legal nesting; it parses, then hits the
	// protected-prefix denial like any other breakpoint-bearing request.
	ok := `{"model":"claude-x","max_tokens":100,"system":[` +
		`{"cache_control":{"type":"ephemeral","ttl":"1h"}},` +
		`{"cache_control":{"type":"ephemeral","ttl":"5m"}}]}`
	got := AnthropicPlanFor(mk(ok))
	if got.Reason != ReasonProtectedPrefixUnproven {
		t.Errorf("1h-then-5m should parse then deny: got %v", got.Reason)
	}
	if got.BaselineBreakpoints1h != 1 || got.BaselineBreakpoints5m != 1 {
		t.Errorf("markers should be counted per ttl: %+v", got)
	}
}

// An unknown cache_control type is refused rather than ignored.
func TestAnthropicPlanRejectsUnknownCacheControl(t *testing.T) {
	js := `{"model":"claude-x","max_tokens":100,"system":[{"cache_control":{"type":"persistent"}}]}`
	in := &AnthropicPlanInput{
		BaselineJSON: js, CandidateJSON: js, Context: anthroContext(),
		BaselineTokens: anthroEvidence(js, 10000), CandidateTokens: anthroEvidence(js, 1000),
	}
	if got := AnthropicPlanFor(in); got.Reason != ReasonInvalidCacheControl {
		t.Errorf("got %v, want invalid_cache_control", got.Reason)
	}
}

// Tools and automatic caching make the cached prefix depend on state the planner
// cannot observe locally.
func TestAnthropicPlanRefusesToolsAndAutomaticCache(t *testing.T) {
	for _, js := range []string{
		`{"model":"claude-x","max_tokens":100,"tools":[]}`,
		`{"model":"claude-x","max_tokens":100,"cache":{"mode":"auto"}}`,
	} {
		in := &AnthropicPlanInput{
			BaselineJSON: js, CandidateJSON: js, Context: anthroContext(),
			BaselineTokens: anthroEvidence(js, 10000), CandidateTokens: anthroEvidence(js, 1000),
		}
		if got := AnthropicPlanFor(in); got.Reason != ReasonUnsupportedCacheLayout {
			t.Errorf("%s: got %v, want unsupported_cache_layout", js, got.Reason)
		}
	}
}

func TestAnthropicPlanPricingContract(t *testing.T) {
	mutations := []func(*AnthropicPrices){
		func(p *AnthropicPrices) { p.CachedReadPerToken = p.InputPerToken },   // not 0.1x
		func(p *AnthropicPrices) { p.CacheWrite5mPerToken = p.InputPerToken }, // not 1.25x
		func(p *AnthropicPrices) { p.CacheWrite1hPerToken = p.InputPerToken }, // not 2x
		func(p *AnthropicPrices) { p.OutputPerToken = 0 },
		// A declared long-context tier with no rates is unusable.
		func(p *AnthropicPrices) { p.LongContextThreshold = 1000 },
	}
	for i, mutate := range mutations {
		in := anthroInput(10000, 1000)
		prices := anthroPrices()
		mutate(&prices)
		in.Context.Prices = prices
		if got := AnthropicPlanFor(in); got.Reason != ReasonPricingUnavailable {
			t.Errorf("pricing mutation %d: got %v", i, got.Reason)
		}
	}
}

// Above the published threshold the long-context rates apply.
func TestAnthropicLongContextPricing(t *testing.T) {
	p := anthroPrices()
	p.LongContextThreshold = 5000
	p.LongInputPerToken = 80
	p.LongOutputPerToken = 160
	in, out, ok := anthropicRequestCost(6000, 10, p)
	if !ok || in != 6000*80 || out != 10*160 {
		t.Errorf("long-context: in=%d out=%d ok=%v", in, out, ok)
	}
	in, out, ok = anthropicRequestCost(1000, 10, p)
	if !ok || in != 1000*40 || out != 10*80 {
		t.Errorf("short-context: in=%d out=%d ok=%v", in, out, ok)
	}
}

func TestAnthropicPlanEvidenceAndIdentity(t *testing.T) {
	if got := AnthropicPlanFor(nil); got.Reason != ReasonInvalidArgument {
		t.Errorf("nil: %v", got.Reason)
	}
	in := anthroInput(10000, 1000)
	in.Context.TokenizerID = 0
	if got := AnthropicPlanFor(in); got.Reason != ReasonInvalidIdentity {
		t.Errorf("unset identity: %v", got.Reason)
	}
	in = anthroInput(10000, 1000)
	in.BaselineTokens.Provider = ProviderOpenAI
	if got := AnthropicPlanFor(in); got.Reason != ReasonTokenizerNotLocalExact {
		t.Errorf("wrong provider: %v", got.Reason)
	}
	in = anthroInput(10000, 1000)
	in.CandidateTokens.Source = TokenSourceRemoteEstimate
	if got := AnthropicPlanFor(in); got.Reason != ReasonRemoteTokenCount {
		t.Errorf("remote estimate: %v", got.Reason)
	}
}

// The baseline is costed with ZERO output: its output is not attributable to the
// transform, so charging it would flatter the candidate.
func TestAnthropicBaselineExcludesOutputCost(t *testing.T) {
	got := AnthropicPlanFor(anthroInput(10000, 1000))
	if got.Scenario.BaselineLower != got.Scenario.BaselineUpper {
		t.Error("baseline bounds should be exact")
	}
	if got.Scenario.BaselineLower != 10000*40 {
		t.Errorf("baseline = %d, want input-only cost", got.Scenario.BaselineLower)
	}
}
