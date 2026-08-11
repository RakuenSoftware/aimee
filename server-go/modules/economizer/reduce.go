package economizer

import "strings"

// The context_reduce orchestrator: composes the levers, applies the freeze cost
// guardrail, tracks the page table, and reports the ledger.
//
// Ported from src/modules/economizer/context_reduce.c.

// charsPerTokenEst is the chars/4 token estimate the module uses throughout.
// A forecast, not an invoice: token COUNTS are caching-independent, but the
// number of tokens a provider actually bills is its tokenizer's business.
const charsPerTokenEst = 4

// FreezeGuardMaxHorizon caps the configured horizon, so a misconfigured long
// horizon cannot justify an unbounded cache-write bet on a single run.
const FreezeGuardMaxHorizon = 5

// Seam identifies where the reducer is running.
type Seam int

const (
	SeamGateway Seam = iota
	SeamDelegate
)

// ReduceReason records why a request did or did not reduce.
type ReduceReason int

const (
	ReduceReasonNone       ReduceReason = iota // no lever enabled at this seam
	ReduceReasonReduced                        // reduction applied
	ReduceReasonMeasured                       // measure-only: metrics computed, not mutated
	ReduceReasonSkipNoGain                     // foldable tokens below MinGainTokens
	ReduceReasonAlready                        // provenance: a prior seam already reduced
)

// PriceRates are the per-token provider rates the freeze guardrail needs.
//
// Passed IN rather than looked up, because the pricing table lives with the
// caller. That keeps this module a pure transformer with no ambient state, which
// is what lets it serve a bus stage without owning a config or pricing
// connection. Priced=false means the model is unknown and the guard fails open.
type PriceRates struct {
	Priced    bool
	InputCost float64
	WriteCost float64
	ReadCost  float64
}

// ReduceConfig mirrors reduce_config_t. All levers default off.
type ReduceConfig struct {
	// Per-lever gates.
	HistoryFold bool // rolling history skeleton + Coordinate Closet
	Compress    bool // boundary-free tool-result BODY compression

	// Per-seam enable: a lever only runs at a seam that is on.
	GatewaySeam  bool
	DelegateSeam bool

	// Shadow mode: compute the ledger but DO NOT mutate the messages.
	MeasureOnly bool

	// Net-gain pre-check: skip fold when foldable tokens < this (0 -> no check).
	MinGainTokens int

	// Freeze cost guardrail. The freeze pins the fold boundary so the reduced
	// prefix stays byte-identical (cache-warm) turn to turn — but a boundary that
	// keeps advancing forces provider cache WRITES that can flip reduction
	// net-negative. When enabled, the boundary is pinned only when the estimated
	// cache-read savings over Horizon reuses cover the one-time write.
	FreezeGuardEnabled bool
	FreezeGuardHorizon int
	Rates              PriceRates

	// §4 page table. Requires state: the index has to outlive a single call.
	RecallEnabled  bool
	RecallTTLTurns int
	// RecallInject appends the hint to the transcript instead of only reporting
	// it. Separate from RecallEnabled on purpose: tracking what was evicted is
	// inert, whereas putting a line in front of the model CHANGES WHAT IT DOES.
	RecallInject bool

	Fold FoldConfig
}

// ReduceState is per-CONVERSATION reducer state, owned by the caller and
// persisted across turns within one conversation. NOT shared across
// agents/sessions — cross-session sharing would leak context.
type ReduceState struct {
	Freeze  FoldFreeze
	Reduced bool // provenance: a second seam re-measures but does NOT re-reduce
	Turn    int
	Recall  *RecallIndex
}

// ReduceResult is the reduced view plus the ledger.
type ReduceResult struct {
	Messages *JSONValue // NEW array when mutated; nil means "use your original"
	Mutated  bool

	Reason ReduceReason

	BaselineTokens int
	ReducedTokens  int
	RemovedTokens  int
	FoldableTokens int

	EstSavedCostFloor   float64
	EstSavedCostCeiling float64

	FoldedMsgs     int
	RetainedMsgs   int
	ReusedBoundary bool
	Epochs         int
	FreezeGuarded  bool
	ClosetEvict    EvictResult

	RecallHint     string
	RecallSurfaced int
}

// FreezeFavorableRates decides whether pinning the boundary pays, from rates
// alone. Scale-invariant, so the prefix size cancels out.
func FreezeFavorableRates(inputCost, writeCost, readCost float64, horizon int) bool {
	if horizon <= 0 {
		horizon = 1 // one future reuse is enough to justify one write
	}
	if horizon > FreezeGuardMaxHorizon {
		horizon = FreezeGuardMaxHorizon
	}

	// Per-reuse saving = paying the cache-READ rate instead of the FRESH input
	// rate. Checked FIRST: with no read discount caching can never pay, so skip
	// the freeze REGARDLESS of write cost — a free write that yields no read
	// benefit is still not worth pinning a boundary for.
	perReuseSaving := inputCost - readCost
	if perReuseSaving <= 0 {
		return false
	}

	// The MARGINAL cost of caching is the write PREMIUM over sending the prefix
	// fresh once (you pay the input rate on the first turn regardless) — NOT the
	// full write cost. Providers with free cache creation have a premium <= 0, so
	// freezing is pure upside.
	writePremium := writeCost - inputCost
	if writePremium <= 0 {
		return true
	}
	return float64(horizon)*perReuseSaving >= writePremium
}

// freezeCostFavorable applies the guardrail for a given prefix size, failing
// open whenever it cannot know better.
func freezeCostFavorable(rates PriceRates, prefixTokens, horizon int) bool {
	if prefixTokens <= 0 {
		return true // nothing to cache -> no churn possible
	}
	if !rates.Priced {
		return true // fail-open: unpriced model -> do not regress onto it
	}
	return FreezeFavorableRates(rates.InputCost, rates.WriteCost, rates.ReadCost, horizon)
}

// nodeTokenEstimate is the chars/4 estimate of a serialized node.
func nodeTokenEstimate(node *JSONValue) int {
	if node == nil {
		return 0
	}
	return len(PrintJSONUnformatted(node)) / charsPerTokenEst
}

// prefixTokenEstimate estimates the first count items of an array.
func prefixTokenEstimate(messages *JSONValue, count int) int {
	if messages == nil || count <= 0 {
		return 0
	}
	total := 0
	for i := 0; i < count && i < messages.Len(); i++ {
		total += len(PrintJSONUnformatted(messages.At(i)))
	}
	return total / charsPerTokenEst
}

// recallTrack harvests coordinates out of the evicted region and surfaces any
// the newest turn re-touches.
func recallTrack(original *JSONValue, evictedCount int, cfg *ReduceConfig, st *ReduceState, out *ReduceResult) {
	if !cfg.RecallEnabled || st == nil || original == nil || !original.IsArray() {
		return
	}
	if st.Recall == nil {
		st.Recall = NewRecallIndex()
	}
	n := original.Len()
	if evictedCount > n {
		evictedCount = n
	}
	for i := 0; i < evictedCount; i++ {
		st.Recall.AddFromText(PrintJSONUnformatted(original.At(i)))
	}
	if st.Recall.Len() == 0 || n == 0 {
		return
	}
	// Detect on the NEWEST turn only: that is the one the agent just wrote, so a
	// coordinate it mentions there is one it is reaching for now.
	lastTxt := PrintJSONUnformatted(original.At(n - 1))
	var hints strings.Builder
	surfaced := st.Recall.Detect(lastTxt, st.Turn, cfg.RecallTTLTurns, &hints)
	if surfaced > 0 && hints.Len() > 0 {
		out.RecallHint = hints.String()
		out.RecallSurfaced = surfaced
	}
}

// recallInject puts the hint in front of the model, at the END of the transcript.
//
// Not the folded prefix and not the system prompt: both are deliberately stable
// so the prompt cache stays warm (§3 freeze), and a per-turn line in either would
// bust the very cache the rest of the economizer exists to protect.
//
// Framed explicitly as a system notice. An unlabelled line appended after the
// user's turn reads as something the USER said, which is both wrong and a way for
// evicted text to put words in their mouth.
//
// Appends rather than splices, so it cannot land between an assistant tool_use
// and its matching tool_result — the one structural mistake that would make the
// request invalid.
func recallInject(reduced *JSONValue, out *ReduceResult) {
	if reduced == nil || !reduced.IsArray() || out.RecallHint == "" {
		return
	}
	var body strings.Builder
	body.WriteString("[context notice — not from the user] Earlier turns were folded " +
		"out of this transcript. You referenced something that went with " +
		"them; it is PAGEABLE, not lost:\n")
	body.WriteString(out.RecallHint)
	note := NewObject()
	note.Set("role", NewString("user"))
	note.Set("content", NewString(body.String()))
	reduced.Append(note)
}

// Reduce composes the levers over messages and reports the ledger.
//
// Never mutates its input. When a lever ran, Messages is a NEW array and Mutated
// is true; otherwise Messages is nil and the caller forwards its original.
func Reduce(messages *JSONValue, systemPrompt string, seam Seam, cfg *ReduceConfig, st *ReduceState) ReduceResult {
	var out ReduceResult
	if messages == nil || !messages.IsArray() {
		out.Reason = ReduceReasonNone
		return out
	}

	// Per-seam gate: the economizer only engages at a seam the operator enabled.
	seamOn := cfg != nil && ((seam == SeamGateway && cfg.GatewaySeam) ||
		(seam == SeamDelegate && cfg.DelegateSeam))
	if !seamOn {
		out.Reason = ReduceReasonNone
		return out
	}

	// Baseline = assembled-context tokens. The system prompt is the immutable
	// prefix zone — counted, never reduced. The +1 is a one-token allowance so a
	// present (even tiny) system prompt is never estimated as zero.
	baseline := nodeTokenEstimate(messages)
	if systemPrompt != "" {
		baseline += len(systemPrompt)/charsPerTokenEst + 1
	}
	out.BaselineTokens = baseline
	out.ReducedTokens = baseline
	out.RemovedTokens = 0

	// Provenance: a request can cross both seams. If a prior seam already
	// reduced, re-measure the baseline but do NOT re-account the opportunity —
	// that saving belongs to the seam that performed it.
	if st != nil && st.Reduced {
		out.Reason = ReduceReasonAlready
		return out
	}

	retained := cfg.Fold.RetainedMsgs
	if retained <= 0 {
		retained = FoldDefaultRetainedMsgs
	}
	n := messages.Len()
	foldableMsgs := 0
	if n > retained {
		foldableMsgs = n - retained
	}
	out.FoldableTokens = prefixTokenEstimate(messages, foldableMsgs)

	work := messages
	var compressedOwned *JSONValue

	// Compress runs FIRST so the fold, when also enabled, sees already-shrunk
	// bodies. Unlike the fold it needs no clean-user-turn boundary, so it engages
	// on autonomous tool-loops where the fold never can.
	if cfg.Compress && !cfg.MeasureOnly {
		cc := FoldConfig{
			Enabled:               true,
			RetainedMsgs:          cfg.Fold.RetainedMsgs,
			ReasoningExcerptBytes: cfg.Fold.ReasoningExcerptBytes,
			CompactHeadBytes:      cfg.Fold.CompactHeadBytes,
			CompactTailBytes:      cfg.Fold.CompactTailBytes,
			Closet:                cfg.Fold.Closet,
		}
		if cr := CompressView(work, &cc); cr.Folded && cr.Messages != nil {
			compressedOwned = cr.Messages
			work = compressedOwned
			out.Mutated = true
			out.Reason = ReduceReasonReduced
			out.FoldedMsgs = cr.FoldedMsgs // bodies compressed; fold may overwrite
			out.ClosetEvict = cr.ClosetEvict
			if st != nil {
				st.Reduced = true
			}
		}
	}

	if cfg.HistoryFold && !cfg.MeasureOnly {
		// Net-gain pre-check: skip when the foldable opportunity is below the
		// operator's round-trip recovery threshold.
		if cfg.MinGainTokens > 0 && out.FoldableTokens < cfg.MinGainTokens {
			if compressedOwned == nil {
				out.Reason = ReduceReasonSkipNoGain
				out.Mutated = false
				out.Messages = nil
				return out
			}
			// compress mutated -> publish below; do not run fold
		} else {
			fc := cfg.Fold
			fc.Enabled = true

			// Freeze cost guardrail: pin the boundary only when the cache-read
			// savings cover the write churn; otherwise this turn re-derives
			// without pinning.
			var freezeArg *FoldFreeze
			if st != nil {
				freezeArg = &st.Freeze
			}
			if freezeArg != nil && cfg.FreezeGuardEnabled &&
				!freezeCostFavorable(cfg.Rates, out.FoldableTokens, cfg.FreezeGuardHorizon) {
				freezeArg = nil
				out.FreezeGuarded = true
			}

			if fr := FoldView(work, &fc, freezeArg); fr.Folded && fr.Messages != nil {
				out.Messages = fr.Messages
				out.Mutated = true
				out.Reason = ReduceReasonReduced
				out.FoldedMsgs = fr.FoldedMsgs
				out.RetainedMsgs = fr.RetainedMsgs
				out.ReusedBoundary = fr.ReusedBoundary
				out.ClosetEvict = fr.ClosetEvict
				if st != nil {
					out.Epochs = st.Freeze.Epochs
					st.Reduced = true
				}
				compressedOwned = nil

				// The fold is the only lever that removes whole MESSAGES, so it is
				// the only one whose eviction the page table must record. Compression
				// shrinks bodies in place — the carrying message stays visible, so
				// nothing has left the prompt to page back in.
				recallTrack(messages, fr.FoldedMsgs, cfg, st, &out)
				if cfg.RecallInject {
					recallInject(out.Messages, &out)
				}
			}
		}
	}

	// Publish the compress-only result when the fold was disabled or no-opped.
	if compressedOwned != nil && out.Messages == nil {
		out.Messages = compressedOwned
	}

	// Recompute the reduced/removed forecast once, over whatever view a lever
	// produced. The system prompt is counted but never reduced.
	if out.Mutated && out.Messages != nil {
		reduced := nodeTokenEstimate(out.Messages)
		if systemPrompt != "" {
			reduced += len(systemPrompt)/charsPerTokenEst + 1
		}
		out.ReducedTokens = reduced
		if baseline > reduced {
			out.RemovedTokens = baseline - reduced
		}
	}

	// Cost forecast bracket: floor prices the basis at the CACHE-READ rate
	// (cache-warm), ceiling at the FRESH input rate (cache-cold). A forecast, never
	// the headline — the invoice-quality number is realized provider spend.
	basis := out.RemovedTokens
	if basis == 0 {
		basis = out.FoldableTokens
	}
	if cfg.Rates.Priced && basis > 0 {
		out.EstSavedCostFloor = cfg.Rates.ReadCost * float64(basis)
		out.EstSavedCostCeiling = cfg.Rates.InputCost * float64(basis)
	}

	// Only the measure path falls through with the reason still unset.
	if out.Reason == ReduceReasonNone {
		out.Reason = ReduceReasonMeasured
		out.Mutated = false
		out.Messages = nil
	}
	return out
}
