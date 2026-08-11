package economizer

import (
	"fmt"
	"strings"
	"testing"
)

// Ported from the state-persistence cases in src/tests/test_context_reduce.c.

func TestStateSerializeRoundTrip(t *testing.T) {
	st := &ReduceState{
		Turn:   7,
		Freeze: FoldFreeze{Active: true, FrozenSplit: 12, TailCapMsgs: 16, Epochs: 3, PrefixDigest: 0xdeadbeefcafe1234},
		Recall: NewRecallIndex(),
	}
	st.Recall.Add("src/modules/git/retry.c")
	st.Recall.Add("memory:8817")
	st.Recall.SetLastTurn("memory:8817", 5)

	blob, ok := SerializeState(st)
	if !ok {
		t.Fatal("serialize failed")
	}

	var restored ReduceState
	if err := RestoreState(&restored, blob); err != nil {
		t.Fatalf("restore: %v", err)
	}
	if restored.Turn != 7 {
		t.Errorf("turn = %d, want 7", restored.Turn)
	}
	fz := restored.Freeze
	if !fz.Active || fz.FrozenSplit != 12 || fz.TailCapMsgs != 16 || fz.Epochs != 3 {
		t.Errorf("freeze round-trip wrong: %+v", fz)
	}
	// The 64-bit digest must survive intact — a JSON number would have lost the
	// low bits, and a wrong digest silently defeats the staleness check.
	if fz.PrefixDigest != 0xdeadbeefcafe1234 {
		t.Errorf("digest = %#x, want 0xdeadbeefcafe1234", fz.PrefixDigest)
	}
	if restored.Recall.Len() != 2 {
		t.Errorf("recall count = %d, want 2", restored.Recall.Len())
	}
	if got := restored.Recall.LastTurn("memory:8817"); got != 5 {
		t.Errorf("residency not restored: last turn = %d, want 5", got)
	}
}

// `Reduced` is per-REQUEST provenance. Restoring it would make the next request
// believe it had already been reduced and skip the work entirely.
func TestStateNeverRestoresReduced(t *testing.T) {
	st := &ReduceState{Turn: 1, Reduced: true, Recall: NewRecallIndex()}
	blob, ok := SerializeState(st)
	if !ok {
		t.Fatal("serialize failed")
	}
	if strings.Contains(blob, "reduced") {
		t.Error("provenance should not be serialized at all")
	}
	restored := ReduceState{Reduced: true}
	if err := RestoreState(&restored, blob); err != nil {
		t.Fatal(err)
	}
	if restored.Reduced {
		t.Error("restore must clear per-request provenance")
	}
}

// A half-applied freeze is worse than none, so a malformed blob must leave the
// caller's state untouched rather than partially written.
func TestStateRestoreAllOrNothing(t *testing.T) {
	for _, bad := range []string{"", "{", "not json", `{"freeze":`} {
		st := ReduceState{Turn: 42, Freeze: FoldFreeze{Active: true, FrozenSplit: 9}}
		if err := RestoreState(&st, bad); err == nil {
			t.Errorf("RestoreState(%q) should have failed", bad)
		}
		if st.Turn != 42 || !st.Freeze.Active || st.Freeze.FrozenSplit != 9 {
			t.Errorf("failed restore clobbered existing state: %+v", st)
		}
	}
}

// The blob is bounded, drops the COLDEST keys first, and says how many it lost.
func TestStateSerializeBoundedAndReportsDrops(t *testing.T) {
	st := &ReduceState{Turn: 100, Recall: NewRecallIndex()}
	// Far more keys than can fit in 6 KB.
	for i := 0; i < 400; i++ {
		key := fmt.Sprintf("/very/long/path/segment/number/%04d/file_with_a_long_name.c", i)
		st.Recall.Add(key)
		// Give the first 5 a recent residency so they rank warmest.
		if i < 5 {
			st.Recall.SetLastTurn(key, 99)
		}
	}
	blob, ok := SerializeState(st)
	if !ok {
		t.Fatal("serialize failed")
	}
	if len(blob) > ReduceStateSerialMax {
		t.Errorf("blob is %d bytes, over the %d cap", len(blob), ReduceStateSerialMax)
	}
	if !strings.Contains(blob, `"recall_dropped":`) || strings.Contains(blob, `"recall_dropped":0`) {
		t.Error("dropping keys must be reported, never silent")
	}
	// The warmest keys survived; a cold one did not.
	for i := 0; i < 5; i++ {
		want := fmt.Sprintf("/very/long/path/segment/number/%04d/", i)
		if !strings.Contains(blob, want) {
			t.Errorf("warmest key %s was dropped ahead of colder ones", want)
		}
	}
	if strings.Contains(blob, "number/0399/") {
		t.Error("a never-surfaced key outranked the warm ones")
	}
}

// The short-key fixture from the C suite, kept so the ported budget logic is
// checked against the case C actually exercises as well as the long-key case it
// drops on the floor.
func TestStateSerializeCFixture(t *testing.T) {
	st := &ReduceState{Recall: NewRecallIndex()}
	for i := 0; i < 400; i++ {
		key := fmt.Sprintf("src/generated/module_%03d/file_%03d.c", i, i)
		st.Recall.Add(key)
		st.Recall.SetLastTurn(key, i) // newer keys have a higher last turn
	}
	blob, ok := SerializeState(st)
	if !ok || len(blob) > ReduceStateSerialMax {
		t.Fatalf("serialize: ok=%v len=%d", ok, len(blob))
	}
	var back ReduceState
	if err := RestoreState(&back, blob); err != nil {
		t.Fatal(err)
	}
	if back.Recall.Len() == 0 || back.Recall.Len() >= 400 {
		t.Errorf("recall count = %d, want genuinely capped", back.Recall.Len())
	}
	// The COLDEST keys are the ones dropped: 399 was surfaced most recently and
	// must survive; 000 must not.
	if !strings.Contains(blob, "file_399") {
		t.Error("the warmest key was dropped")
	}
	if strings.Contains(blob, "file_000") {
		t.Error("the coldest key survived ahead of warmer ones")
	}
}

// The payoff: a coordinate evicted in an earlier RUN is still pageable in the
// next one, which is the whole reason state is persisted.
func TestStateRecallSurvivesAcrossRuns(t *testing.T) {
	const coord = "src/modules/git/retry.c"

	// Run 1: the coordinate is deep in the prefix and gets folded away.
	run1 := makeMessages(20)
	run1.At(1).Set("content", NewString("the retry backoff lives in "+coord+" and needs care"))
	cfg := &ReduceConfig{DelegateSeam: true, HistoryFold: true, RecallEnabled: true,
		Fold: FoldConfig{Closet: ClosetConfig{Enabled: true}}}
	st1 := &ReduceState{Turn: 3, Recall: NewRecallIndex()}
	if out := Reduce(run1, "sys", SeamDelegate, cfg, st1); !out.Mutated {
		t.Fatal("run 1 did not fold")
	}
	if st1.Recall.Len() == 0 {
		t.Fatal("the page table did not see the eviction")
	}

	// The run ends: state is persisted.
	blob, ok := SerializeState(st1)
	if !ok || !strings.Contains(blob, coord) {
		t.Fatalf("coordinate not persisted: ok=%v", ok)
	}

	// Run 2: a fresh process restores that state.
	var st2 ReduceState
	if err := RestoreState(&st2, blob); err != nil {
		t.Fatal(err)
	}
	st2.Turn = 9 // far enough ahead that the anti-thrash TTL cannot suppress it

	// This transcript never mentions the coordinate except in its newest turn,
	// which is retained verbatim and therefore never evicted — so nothing HERE
	// could have put the key in the table.
	run2 := makeMessages(20)
	run2.At(run2.Len()-1).Set("content", NewString(
		"remind me what we concluded about "+coord+" before"))
	out := Reduce(run2, "sys", SeamDelegate, cfg, &st2)
	if out.RecallSurfaced < 1 || !strings.Contains(out.RecallHint, coord) {
		t.Fatalf("an eviction from the PREVIOUS run should still be pageable: %q", out.RecallHint)
	}

	// Negative half: with no restored state, the same run 2 produces NO hint.
	// Without this the test above could pass on a bug that hinted for any
	// mentioned path.
	fresh := &ReduceState{Turn: 9, Recall: NewRecallIndex()}
	run2b := makeMessages(20)
	run2b.At(run2b.Len()-1).Set("content", NewString(
		"remind me what we concluded about "+coord+" before"))
	if off := Reduce(run2b, "sys", SeamDelegate, cfg, fresh); off.RecallHint != "" {
		t.Errorf("no restored state must mean no hint, got %q", off.RecallHint)
	}
}
