package economizer

import (
	"fmt"
	"testing"
)

// GPT-5.6 published rates, in fixed-point money per token, satisfying the
// relationships pricesValid enforces.
func okPrices() OpenAIPrices {
	const input = 40 // divisible by 10 and by 4
	return OpenAIPrices{
		InputPerToken:      input,
		CachedReadPerToken: input / 10,
		CacheWritePerToken: (input / 4) * 5,
		OutputPerToken:     80,
	}
}

func okContext() *OpenAIContext {
	return &OpenAIContext{
		PinnedModel: "gpt-5.6", ModelSnapshotID: 11, TokenizerID: 22,
		PricingTableID: 33, ContractVersions: 44,
		TokenizerGuardTokens: 100, SafetyMargin: 0, Prices: okPrices(),
	}
}

func req(maxOut int) string {
	return fmt.Sprintf(`{"model":"gpt-5.6","prompt_cache":{"mode":"explicit"},"max_output_tokens":%d}`, maxOut)
}

func evidence(json string, tokens uint64) *TokenEvidence {
	return &TokenEvidence{
		Provider: ProviderOpenAI, EndpointID: uint32(OpenAIResponses),
		ModelSnapshotID: 11, TokenizerID: 22,
		SerializedSize: len(json), InputTokens: tokens, Source: TokenSourceLocalExact,
	}
}

func planInput(baseTokens, candTokens uint64) *OpenAIPlanInput {
	b, c := req(100), req(100)
	return &OpenAIPlanInput{
		BaselineJSON: b, CandidateJSON: c, Context: okContext(),
		BaselineTokens: evidence(b, baseTokens), CandidateTokens: evidence(c, candTokens),
		Endpoint: OpenAIResponses,
	}
}

func TestOpenAIPlanProvesACheaperCandidate(t *testing.T) {
	got := OpenAIGPT56Plan(planInput(10000, 1000))
	if got.CostVerdict != CostProven || got.Reason != ReasonProofAccepted {
		t.Fatalf("got %v/%v, want CostProven/proof_accepted", got.CostVerdict, got.Reason)
	}
	if got.Scenario.BaselineLower <= got.Scenario.CandidateUpper {
		t.Error("the proven scenario is not actually cheaper")
	}
}

// A candidate that is not cheaper is REJECTED, not quietly allowed.
func TestOpenAIPlanRejectsNonCheaper(t *testing.T) {
	got := OpenAIGPT56Plan(planInput(1000, 10000))
	if got.CostVerdict != CostRejected || got.Reason != ReasonNotStrictlyCheaper {
		t.Errorf("got %v/%v, want CostRejected/not_strictly_cheaper", got.CostVerdict, got.Reason)
	}
}

// Token evidence must describe THIS request: a count measured over different
// bytes, a different model or tokenizer, or a non-local source is refused.
func TestOpenAIPlanRequiresBindingLocalEvidence(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*OpenAIPlanInput)
		want   Reason
	}{
		{"missing evidence", func(p *OpenAIPlanInput) { p.BaselineTokens = nil }, ReasonTokenizerNotLocalExact},
		{"wrong size", func(p *OpenAIPlanInput) { p.BaselineTokens.SerializedSize++ }, ReasonTokenizerNotLocalExact},
		{"wrong model", func(p *OpenAIPlanInput) { p.BaselineTokens.ModelSnapshotID = 999 }, ReasonTokenizerNotLocalExact},
		{"wrong tokenizer", func(p *OpenAIPlanInput) { p.BaselineTokens.TokenizerID = 999 }, ReasonTokenizerNotLocalExact},
		{"wrong endpoint", func(p *OpenAIPlanInput) { p.BaselineTokens.EndpointID = 99 }, ReasonTokenizerNotLocalExact},
		{"wrong provider", func(p *OpenAIPlanInput) { p.BaselineTokens.Provider = ProviderAnthropic }, ReasonTokenizerNotLocalExact},
		{"remote estimate", func(p *OpenAIPlanInput) { p.BaselineTokens.Source = TokenSourceRemoteEstimate }, ReasonRemoteTokenCount},
		// Exact but UNPRICED is still refused: obtaining the count may add
		// unknown cost to the candidate path.
		{"remote exact unpriced", func(p *OpenAIPlanInput) { p.BaselineTokens.Source = TokenSourceRemoteExactUnpriced }, ReasonRemoteTokenCountUnpriced},
	}
	for _, c := range cases {
		in := planInput(10000, 1000)
		c.mutate(in)
		if got := OpenAIGPT56Plan(in); got.Reason != c.want {
			t.Errorf("%s: got %v, want %v", c.name, got.Reason, c.want)
		}
	}
}

// The pricing table must satisfy the published RELATIONSHIPS, not merely be
// positive: a drifted table is no longer the one this planner was proven against.
func TestOpenAIPlanRequiresRecognisedPricing(t *testing.T) {
	mutations := []func(*OpenAIPrices){
		func(p *OpenAIPrices) { p.CachedReadPerToken = p.InputPerToken }, // not a tenth
		func(p *OpenAIPrices) { p.CacheWritePerToken = p.InputPerToken }, // not 1.25x
		func(p *OpenAIPrices) { p.OutputPerToken = 81 },                  // odd
		func(p *OpenAIPrices) { p.InputPerToken = 0 },
		func(p *OpenAIPrices) { p.InputPerToken = 42 }, // not divisible by 10
	}
	for i, mutate := range mutations {
		in := planInput(10000, 1000)
		prices := okPrices()
		mutate(&prices)
		in.Context.Prices = prices
		if got := OpenAIGPT56Plan(in); got.Reason != ReasonPricingUnavailable {
			t.Errorf("pricing mutation %d: got %v, want pricing_unavailable", i, got.Reason)
		}
	}
}

// Near the long-context boundary a small tokenizer disagreement flips the price
// tier, so the planner refuses rather than betting on which side it lands.
func TestOpenAIPlanRefusesInsideTheGuardBand(t *testing.T) {
	in := planInput(OpenAIGPT56LongContextThreshold+50, 1000)
	if got := OpenAIGPT56Plan(in); got.Reason != ReasonTokenGuardBand {
		t.Errorf("just above the boundary: got %v, want token_guard_band", got.Reason)
	}
	in = planInput(10000, OpenAIGPT56LongContextThreshold-50)
	if got := OpenAIGPT56Plan(in); got.Reason != ReasonTokenGuardBand {
		t.Errorf("just below the boundary: got %v, want token_guard_band", got.Reason)
	}
}

// Cache layout must be explicit, and the two paths must share a cache identity
// or their costs are not comparable.
func TestOpenAIPlanCacheLayoutRules(t *testing.T) {
	mk := func(baseline, candidate string) *OpenAIPlanInput {
		return &OpenAIPlanInput{
			BaselineJSON: baseline, CandidateJSON: candidate, Context: okContext(),
			BaselineTokens: evidence(baseline, 10000), CandidateTokens: evidence(candidate, 1000),
			Endpoint: OpenAIResponses,
		}
	}
	noCache := `{"model":"gpt-5.6","max_output_tokens":100}`
	if got := OpenAIGPT56Plan(mk(noCache, noCache)); got.Reason != ReasonUnsupportedCacheLayout {
		t.Errorf("implicit cache mode: got %v", got.Reason)
	}

	keyed := `{"model":"gpt-5.6","prompt_cache":{"mode":"explicit","key":"a"},"max_output_tokens":100}`
	if got := OpenAIGPT56Plan(mk(keyed, req(100))); got.Reason != ReasonUnsupportedCacheLayout {
		t.Errorf("mismatched cache key presence: got %v", got.Reason)
	}

	// Differing output bounds make the candidate's ceiling unknowable.
	if got := OpenAIGPT56Plan(mk(req(100), req(200))); got.Reason != ReasonOutputBoundUnavailable {
		t.Errorf("mismatched output bound: got %v", got.Reason)
	}

	// Breakpoints are parsed but DENIED: proving a protected prefix needs exact
	// serialized ranges, which are not implemented.
	bp := `{"model":"gpt-5.6","prompt_cache":{"mode":"explicit"},"max_output_tokens":100,` +
		`"input":[{"cache_control":{"type":"ephemeral"}}]}`
	got := OpenAIGPT56Plan(mk(bp, bp))
	if got.Reason != ReasonProtectedPrefixUnproven {
		t.Errorf("breakpoints: got %v, want protected_prefix_unproven", got.Reason)
	}
	if got.BaselineBreakpoints != 1 || got.CandidateBreakpoints != 1 {
		t.Errorf("breakpoints should still be counted and reported: %+v", got)
	}
}

// A request naming a different model, or carrying duplicate keys, cannot be
// costed: a proof against one reading says nothing about what the provider runs.
func TestOpenAIPlanRejectsBadRequestShape(t *testing.T) {
	mk := func(js string) *OpenAIPlanInput {
		return &OpenAIPlanInput{
			BaselineJSON: js, CandidateJSON: js, Context: okContext(),
			BaselineTokens: evidence(js, 10000), CandidateTokens: evidence(js, 1000),
			Endpoint: OpenAIResponses,
		}
	}
	for _, js := range []string{
		`{"model":"gpt-4o","prompt_cache":{"mode":"explicit"},"max_output_tokens":100}`,
		`{"model":"gpt-5.6","model":"gpt-5.6","prompt_cache":{"mode":"explicit"}}`,
		`not json`,
	} {
		if got := OpenAIGPT56Plan(mk(js)); got.Reason != ReasonInvalidRequestShape {
			t.Errorf("%q: got %v, want invalid_request_shape", js, got.Reason)
		}
	}
}

func TestOpenAIPlanArgumentAndIdentityChecks(t *testing.T) {
	if got := OpenAIGPT56Plan(nil); got.Reason != ReasonInvalidArgument {
		t.Errorf("nil input: %v", got.Reason)
	}
	in := planInput(10000, 1000)
	in.Context.ModelSnapshotID = 0
	if got := OpenAIGPT56Plan(in); got.Reason != ReasonInvalidIdentity {
		t.Errorf("unset identity: %v", got.Reason)
	}
	in = planInput(10000, 1000)
	in.Endpoint = 99
	if got := OpenAIGPT56Plan(in); got.Reason != ReasonUnsupportedEndpoint {
		t.Errorf("bad endpoint: %v", got.Reason)
	}
}

// Long-context pricing doubles input and multiplies output by 1.5.
func TestOpenAILongContextPricing(t *testing.T) {
	short, ok := inputCost(1000, 40)
	if !ok || short != 40000 {
		t.Errorf("short-context input = %d, want 40000", short)
	}
	long, ok := inputCost(OpenAIGPT56LongContextThreshold+1, 40)
	if !ok {
		t.Fatal("long-context input overflowed")
	}
	if long != 2*Money(OpenAIGPT56LongContextThreshold+1)*40 {
		t.Errorf("long-context input = %d, want doubled", long)
	}
	out, ok := outputCost(100, 80, true)
	if !ok || out != 100*120 {
		t.Errorf("long-context output = %d, want 12000 (1.5x)", out)
	}
}
