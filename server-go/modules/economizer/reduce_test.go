package economizer

import (
	"fmt"
	"strings"
	"testing"
)

// Ported from src/tests/test_context_reduce.c.

func makeMessages(rounds int) *JSONValue {
	arr := NewArray()
	for i := 0; i < rounds; i++ {
		arr.Append(mkUser("please read the file and summarize the relevant section in detail"))
		arr.Append(mkAsst("here is a fairly long assistant turn that adds bytes to the transcript " +
			"so the fold-eligible prefix carries real token volume across many turns of the session"))
	}
	return arr
}

// Append one realistic turn: a user ask, an assistant tool_call, a bulky tool
// result, and an assistant reply — so BOTH levers have something to do.
func appendTurn(m *JSONValue, k int) {
	id := fmt.Sprintf("call_%03d", k)
	m.Append(mkUser("keep going on the migration and report what changed"))

	a := NewObject()
	a.Set("role", NewString("assistant"))
	tcs := NewArray()
	tc := NewObject()
	tc.Set("id", NewString(id))
	tc.Set("type", NewString("function"))
	fn := NewObject()
	fn.Set("name", NewString("read_file"))
	fn.Set("arguments", NewString("{}"))
	tc.Set("function", fn)
	tcs.Append(tc)
	a.Set("tool_calls", tcs)
	m.Append(a)

	var body strings.Builder
	for body.Len()+48 < 900-96 {
		body.WriteString("filler output bytes here and on; ")
	}
	fmt.Fprintf(&body, "tail at /work/src/stage_%d.c done", k)
	tr := NewObject()
	tr.Set("role", NewString("tool"))
	tr.Set("tool_call_id", NewString(id))
	tr.Set("content", NewString(body.String()))
	m.Append(tr)

	m.Append(mkAsst("read the stage file and applied the change; moving to the next one " +
		"now that the previous edit is confirmed good"))
}

// THE CACHE CLAIM, through the composed reducer. This is the Go form of the test
// that found the compress-defeats-freeze bug (#2552).
//
// The freeze exists so the reduced PREFIX stays byte-identical turn to turn and
// the provider prompt cache keeps hitting. fold_test.go proves that for the fold
// lever alone — but production stacks compress AHEAD of fold and the fold digests
// the COMPRESSED view, so a compressor whose output for the prefix region drifts
// as the retained band slides would silently epoch the freeze and bust the cache
// with every lever still reporting success.
//
// The invariant, stated honestly: while the epoch counter holds, the emitted
// prefix must be byte-identical; a changed prefix is legal ONLY on an epoch
// advance. Bytes are compared, not token counts — a cache hit is a bytewise
// prefix match, so anything weaker would pass on a prefix that merely looks the
// same.
//
// RecallInject is deliberately ON: tail placement is claimed to be cache-safe. If
// the hint ever landed in the prefix instead, this fails.
func TestReducePrefixStableAcrossTurns(t *testing.T) {
	m := NewArray()
	for k := 0; k < 8; k++ {
		appendTurn(m, k)
	}

	cfg := &ReduceConfig{
		DelegateSeam:  true,
		HistoryFold:   true,
		Compress:      true,
		RecallEnabled: true,
		RecallInject:  true,
		Fold: FoldConfig{
			Closet:           ClosetConfig{Enabled: true},
			CompactHeadBytes: 40,
		},
	}
	st := &ReduceState{Freeze: FoldFreeze{TailCapMsgs: 16}, Recall: NewRecallIndex()}

	prevPrefix := ""
	prevEpochs := -1
	reuses, epochs := 0, 0

	for turn := 0; turn < 14; turn++ {
		st.Turn = turn
		out := Reduce(m, "sys", SeamDelegate, cfg, st)
		st.Reduced = false // next turn is a fresh request, not a second seam

		if out.Mutated && out.Messages != nil {
			prefix := PrintJSONUnformatted(out.Messages.At(0))
			if prevPrefix != "" {
				if out.Epochs == prevEpochs {
					// No epoch advance -> the cache MUST still be warm.
					if prefix != prevPrefix {
						t.Fatalf("turn %d: prefix changed without an epoch advance", turn)
					}
					reuses++
				} else {
					if out.Epochs < prevEpochs {
						t.Fatalf("turn %d: epochs went backwards", turn)
					}
					epochs++
				}
			}
			prevPrefix = prefix
			prevEpochs = out.Epochs
		}
		appendTurn(m, 100+turn)
	}

	// Guard the guard: a run where the fold never engaged, or never held a
	// boundary across a turn, would satisfy every assertion above vacuously.
	if reuses == 0 {
		t.Fatal("no cache-warm reuse happened — the assertions above were vacuous")
	}
	t.Logf("%d cache-warm reuses, %d epoch advance(s)", reuses, epochs)
}

// The freeze cost guardrail, ported from test_freeze_guard's rate arithmetic.
func TestReduceFreezeFavorableRates(t *testing.T) {
	cases := []struct {
		name               string
		input, write, read float64
		horizon            int
		want               bool
	}{
		{"anthropic-like at H=1", 3.0, 3.75, 0.30, 1, true},
		{"free write, no read discount -> skip", 1.0, 0.0, 1.0, 1, false},
		{"free write with read discount", 1.0, 0.0, 0.10, 1, true},
		{"premium far exceeds saving", 1.0, 10.0, 0.10, 1, false},
		{"horizon clamp cannot cover it", 1.0, 10.0, 0.10, 9999, false},
		{"mild premium missed at H=1", 1.0, 3.0, 0.10, 1, false},
		{"same premium covered at H=3", 1.0, 3.0, 0.10, 3, true},
	}
	for _, c := range cases {
		if got := FreezeFavorableRates(c.input, c.write, c.read, c.horizon); got != c.want {
			t.Errorf("%s: got %v, want %v", c.name, got, c.want)
		}
	}
}

// The guard fails OPEN: an unpriced model or a zero-size prefix must not disable
// a freeze that would otherwise run.
func TestReduceFreezeGuardFailsOpen(t *testing.T) {
	if !freezeCostFavorable(PriceRates{Priced: false}, 100000, 1) {
		t.Error("unpriced model must fail open")
	}
	if !freezeCostFavorable(PriceRates{Priced: true, InputCost: 1, WriteCost: 99, ReadCost: 1}, 0, 1) {
		t.Error("zero prefix must fail open")
	}
}

// A seam that is not enabled is a true no-op: no measurement, no mutation.
func TestReduceSeamGating(t *testing.T) {
	m := makeMessages(20)
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true}
	out := Reduce(m, "sys", SeamGateway, cfg, nil)
	if out.Reason != ReduceReasonNone || out.Mutated || out.BaselineTokens != 0 {
		t.Errorf("disabled seam must be a true no-op: %+v", out)
	}
	if out2 := Reduce(m, "sys", SeamDelegate, nil, nil); out2.Reason != ReduceReasonNone {
		t.Error("nil config must be a no-op")
	}
}

// measure_only computes the ledger without touching the transcript.
func TestReduceMeasureOnly(t *testing.T) {
	m := makeMessages(20)
	before := PrintJSONUnformatted(m)
	cfg := &ReduceConfig{DelegateSeam: true, MeasureOnly: true, HistoryFold: true}
	out := Reduce(m, "system prompt here", SeamDelegate, cfg, nil)
	if out.Reason != ReduceReasonMeasured || out.Mutated || out.Messages != nil {
		t.Errorf("measure-only must not mutate: %+v", out)
	}
	if out.BaselineTokens <= 0 || out.ReducedTokens != out.BaselineTokens || out.RemovedTokens != 0 {
		t.Errorf("measure-only ledger wrong: %+v", out)
	}
	if out.FoldableTokens <= 0 {
		t.Error("measure-only should still report the foldable opportunity")
	}
	if PrintJSONUnformatted(m) != before {
		t.Error("measure-only mutated the input")
	}
}

// Provenance: a second seam re-measures but does not re-reduce.
func TestReduceProvenanceAlready(t *testing.T) {
	m := makeMessages(20)
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true}
	st := &ReduceState{Reduced: true}
	out := Reduce(m, "sys", SeamDelegate, cfg, st)
	if out.Reason != ReduceReasonAlready || out.Mutated {
		t.Errorf("second seam must not re-reduce: %+v", out)
	}
	if out.BaselineTokens <= 0 {
		t.Error("second seam should still re-measure the baseline")
	}
	if out.FoldableTokens != 0 {
		t.Error("second seam must not re-account the opportunity")
	}
}

// Below the net-gain threshold, the fold skips without mutating.
func TestReduceSkipNoGain(t *testing.T) {
	m := makeMessages(20)
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true, MinGainTokens: 1000000}
	out := Reduce(m, "sys", SeamDelegate, cfg, nil)
	if out.Reason != ReduceReasonSkipNoGain || out.Mutated || out.Messages != nil {
		t.Errorf("below min_gain must skip cleanly: %+v", out)
	}
}

// The fold actually reduces, and the ledger reports a real saving.
func TestReduceHistoryFoldReduces(t *testing.T) {
	m := makeMessages(20)
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true,
		Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}}}
	out := Reduce(m, "sys", SeamDelegate, cfg, &ReduceState{})
	if out.Reason != ReduceReasonReduced || !out.Mutated || out.Messages == nil {
		t.Fatalf("fold should have reduced: %+v", out)
	}
	if out.ReducedTokens >= out.BaselineTokens || out.RemovedTokens == 0 {
		t.Errorf("ledger shows no saving: %+v", out)
	}
	if out.FoldedMsgs <= 0 {
		t.Error("no messages folded")
	}
}

// A coordinate evicted by the fold and re-touched by the newest turn surfaces as
// a pageable hint; with recall disabled the lever is inert.
func TestReduceRecallHintOnRetouch(t *testing.T) {
	const coord = "src/modules/git/retry.c"
	build := func() *JSONValue {
		m := makeMessages(20)
		m.At(1).Set("content", NewString(
			"the retry backoff lives in "+coord+" and needs care"))
		m.At(m.Len()-1).Set("content", NewString(
			"remind me what we concluded about "+coord+" before"))
		return m
	}
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true, RecallEnabled: true,
		Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}}}
	st := &ReduceState{Turn: 9, Recall: NewRecallIndex()}
	out := Reduce(build(), "sys", SeamDelegate, cfg, st)
	if out.RecallSurfaced < 1 || !strings.Contains(out.RecallHint, coord) {
		t.Fatalf("expected a recall hint for %s, got %q", coord, out.RecallHint)
	}
	if !strings.Contains(out.RecallHint, "code_span_get") {
		t.Error("hint should name how to page the coordinate back in")
	}

	// Negative half: disabled recall produces no hint at all.
	cfgOff := *cfg
	cfgOff.RecallEnabled = false
	if off := Reduce(build(), "sys", SeamDelegate, &cfgOff, &ReduceState{Turn: 9}); off.RecallHint != "" {
		t.Errorf("recall disabled but hinted: %q", off.RecallHint)
	}
}

// The injected notice goes at the TAIL and is labelled as not coming from the
// user — an unlabelled line after the user's turn reads as something they said.
func TestReduceRecallInjectAppendsNotice(t *testing.T) {
	const coord = "src/modules/git/retry.c"
	m := makeMessages(20)
	m.At(1).Set("content", NewString("the retry backoff lives in "+coord+" and needs care"))
	m.At(m.Len()-1).Set("content", NewString("remind me about "+coord+" before"))

	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true, RecallEnabled: true,
		RecallInject: true, Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}}}
	st := &ReduceState{Turn: 9, Recall: NewRecallIndex()}
	out := Reduce(m, "sys", SeamDelegate, cfg, st)
	if out.Messages == nil || out.RecallSurfaced < 1 {
		t.Fatal("expected a reduced view carrying a hint")
	}
	last := out.Messages.At(out.Messages.Len() - 1)
	body := last.GetString("content")
	if !strings.Contains(body, "context notice — not from the user") {
		t.Errorf("notice is not labelled as a system notice: %q", body)
	}
	if !strings.Contains(body, coord) {
		t.Error("notice does not carry the coordinate")
	}
	if strings.Contains(out.Messages.At(0).GetString("content"), "context notice") {
		t.Error("the notice must not land in the cache-stable prefix")
	}
}

// Compress engages where the fold cannot: a tool loop has no clean user-turn
// boundary, so fold-only must no-op while compress still shrinks the bodies.
func TestReduceCompressEngagesWhereFoldCannot(t *testing.T) {
	foldOnly := &ReduceConfig{DelegateSeam: true, HistoryFold: true,
		Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}}}
	if out := Reduce(compressFixture(), "sys", SeamDelegate, foldOnly, &ReduceState{}); out.Mutated {
		t.Error("fold found a boundary in a tool loop where none exists")
	}

	compressOnly := &ReduceConfig{DelegateSeam: true, Compress: true,
		Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}, CompactHeadBytes: 40}}
	m := compressFixture()
	origCount := m.Len()
	out := Reduce(m, "sys", SeamDelegate, compressOnly, &ReduceState{})
	if out.Reason != ReduceReasonReduced || !out.Mutated || out.Messages == nil {
		t.Fatalf("compress should have engaged: %+v", out)
	}
	if out.ReducedTokens >= out.BaselineTokens {
		t.Error("compress did not shrink the transcript")
	}
	if m.Len() != origCount {
		t.Error("compress mutated the original")
	}
	// A buried path is conserved (the Coordinate Closet rode along).
	if !strings.Contains(PrintJSONUnformatted(out.Messages), "/work/src/stage_2.c") {
		t.Error("a buried identifier was lost")
	}
}
