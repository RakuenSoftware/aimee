package economizer

import "testing"

func reducedResult() *ReduceResult {
	msgs := NewArray()
	msgs.Append(mkUser("hi"))
	return &ReduceResult{
		Messages: msgs, Mutated: true, Reason: ReduceReasonReduced,
		BaselineTokens: 1000, ReducedTokens: 400,
	}
}

func cleanRepair(*JSONValue) int { return 0 }

func TestGWShouldApplyAcceptsAGenuineReduction(t *testing.T) {
	if got := GWShouldApply(true, reducedResult(), ReduceErrNone, cleanRepair); got != GWBypassNone {
		t.Errorf("got %v, want none", got)
	}
}

// Only an APPLIED reduction is worth mutating for. Measure-only, already-reduced
// and skipped all bypass.
func TestGWShouldApplyNoOpCases(t *testing.T) {
	cases := []struct {
		name   string
		mutate func(*ReduceResult)
	}{
		{"no messages", func(r *ReduceResult) { r.Messages = nil }},
		{"not mutated", func(r *ReduceResult) { r.Mutated = false }},
		{"measured", func(r *ReduceResult) { r.Reason = ReduceReasonMeasured }},
		{"already", func(r *ReduceResult) { r.Reason = ReduceReasonAlready }},
		{"skip no gain", func(r *ReduceResult) { r.Reason = ReduceReasonSkipNoGain }},
		// A reduction that did not shrink is not worth the blast radius.
		{"no net shrink", func(r *ReduceResult) { r.ReducedTokens = r.BaselineTokens }},
		{"grew", func(r *ReduceResult) { r.ReducedTokens = r.BaselineTokens + 1 }},
	}
	for _, c := range cases {
		res := reducedResult()
		c.mutate(res)
		if got := GWShouldApply(true, res, ReduceErrNone, cleanRepair); got != GWBypassNoOp {
			t.Errorf("%s: got %v, want no_op", c.name, got)
		}
	}
}

// A reducer failure maps to its specific class, and an unnamed failure becomes
// an internal assertion rather than being waved through.
func TestGWShouldApplyReducerErrors(t *testing.T) {
	cases := map[ReduceError]GWBypassReason{
		ReduceErrAllocFailed:       GWBypassReduceAllocFailed,
		ReduceErrParseFailed:       GWBypassReduceParseFailed,
		ReduceErrFormatUnsupported: GWBypassReduceFormatUnsupported,
		ReduceErrInternalAssertion: GWBypassReduceInternalAssertion,
		ReduceErrNone:              GWBypassReduceInternalAssertion,
	}
	for err, want := range cases {
		if got := GWShouldApply(false, reducedResult(), err, cleanRepair); got != want {
			t.Errorf("err %v: got %v, want %v", err, got, want)
		}
	}
	// A nil result cannot be inspected, so it is an internal assertion.
	if got := GWShouldApply(true, nil, ReduceErrNone, cleanRepair); got != GWBypassReduceInternalAssertion {
		t.Errorf("nil result: got %v", got)
	}
}

// THE STRUCTURAL GUARD: any non-zero repair count means the reduced view was not
// clean, so it must never be dispatched. Negative codes count too — a view we
// cannot prove clean is not clean.
func TestGWShouldApplyStructuralViolation(t *testing.T) {
	for _, repairs := range []int{1, 5, -1} {
		check := func(*JSONValue) int { return repairs }
		if got := GWShouldApply(true, reducedResult(), ReduceErrNone, check); got != GWBypassStructuralViolation {
			t.Errorf("repairs=%d: got %v, want structural_violation", repairs, got)
		}
	}
}

// The structural check must run on a COPY: the reduced result must come back
// untouched whatever the checker does to what it is handed.
func TestGWStructuralCheckDoesNotMutateResult(t *testing.T) {
	res := reducedResult()
	before := PrintJSONUnformatted(res.Messages)
	vandal := func(m *JSONValue) int {
		m.Items = nil // destroy the copy
		return 0
	}
	if got := GWShouldApply(true, res, ReduceErrNone, vandal); got != GWBypassNone {
		t.Fatalf("got %v", got)
	}
	if after := PrintJSONUnformatted(res.Messages); after != before {
		t.Error("the structural check was run on the result rather than a copy")
	}
}

func TestGWSnapshotIsIndependent(t *testing.T) {
	msgs := NewArray()
	msgs.Append(mkUser("original"))
	snap := GWSnapshotMessages(msgs)
	msgs.At(0).Set("content", NewString("mutated"))
	if snap.At(0).GetString("content") != "original" {
		t.Error("the snapshot shares state with the original — restore would not work")
	}
	if GWSnapshotMessages(nil) != nil {
		t.Error("a nil array should snapshot to nil")
	}
	if GWSnapshotTokenCount(msgs) <= 0 {
		t.Error("token count should be positive for a non-empty array")
	}
}

func TestGWReplaceMessages(t *testing.T) {
	container := NewObject()
	container.Set("messages", NewArray())
	reduced := NewArray()
	reduced.Append(mkUser("new"))
	if !GWReplaceMessages(container, "messages", reduced) {
		t.Fatal("replace failed")
	}
	if container.Get("messages").At(0).GetString("content") != "new" {
		t.Error("the reduced array was not installed")
	}
	// An absent key is added rather than refused.
	if !GWReplaceMessages(container, "input", reduced) {
		t.Error("adding an absent key should succeed")
	}
	// Bad arguments are refused, leaving the container intact.
	for _, bad := range []struct {
		c *JSONValue
		k string
		r *JSONValue
	}{
		{nil, "messages", reduced},
		{container, "", reduced},
		{container, "messages", nil},
	} {
		if GWReplaceMessages(bad.c, bad.k, bad.r) {
			t.Error("bad arguments should be refused")
		}
	}
}

func TestGWProvenance(t *testing.T) {
	st := &ReduceState{}
	GWProvenanceMarkReduced(st)
	if !st.Reduced {
		t.Error("mark did not set provenance")
	}
	GWProvenanceClear(st)
	if st.Reduced {
		t.Error("clear did not reset provenance")
	}
	GWProvenanceMarkReduced(nil) // must not panic
	GWProvenanceClear(nil)
}

func TestGWBypassReasonStrings(t *testing.T) {
	cases := map[GWBypassReason]string{
		GWBypassNone: "none", GWBypassNoOp: "no_op",
		GWBypassStructuralViolation: "structural_violation",
		GWBypassSnapshotOOM:         "snapshot_oom",
		GWBypassReplaceFailed:       "replace_failed",
		GWBypassConstructFailed:     "construct_failed",
		GWBypassReduceAllocFailed:   "reduce_alloc_failed",
	}
	for r, want := range cases {
		if got := r.String(); got != want {
			t.Errorf("%d.String() = %q, want %q", r, got, want)
		}
	}
}
