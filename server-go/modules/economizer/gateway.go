package economizer

// Gateway mutation helpers: the provider-agnostic decision and snapshot/replace
// logic that lets the inbound gateway APPLY context reduction to a live request.
//
// Ported from src/modules/economizer/gateway_mutate.c.
//
// THE CORE INVARIANT: never dispatch a reduced payload that cannot be restored
// to the pristine original. Every branch below exists to uphold it, and every
// ambiguous case bypasses rather than mutates — a gateway that mangles a live
// customer request is far worse than one that fails to save tokens.

// GWBypassReason records why the gateway did NOT apply the reduced payload.
// GWBypassNone means "apply".
type GWBypassReason int

const (
	GWBypassNone GWBypassReason = iota
	// Reducer internal-error class.
	GWBypassReduceAllocFailed
	GWBypassReduceParseFailed
	GWBypassReduceInternalAssertion
	GWBypassReduceFormatUnsupported
	// Gateway-side outcomes.
	GWBypassNoOp                // nothing changed / not a net shrink / not REDUCED
	GWBypassStructuralViolation // an orphaned tool pair was found
	GWBypassSnapshotOOM         // a required deep copy failed
	GWBypassReplaceFailed       // installing the reduced array failed
	GWBypassConstructFailed     // a post-decision construction step failed
)

var gwBypassNames = map[GWBypassReason]string{
	GWBypassNone:                    "none",
	GWBypassReduceAllocFailed:       "reduce_alloc_failed",
	GWBypassReduceParseFailed:       "reduce_parse_failed",
	GWBypassReduceInternalAssertion: "reduce_internal_assertion",
	GWBypassReduceFormatUnsupported: "reduce_format_unsupported",
	GWBypassNoOp:                    "no_op",
	GWBypassStructuralViolation:     "structural_violation",
	GWBypassSnapshotOOM:             "snapshot_oom",
	GWBypassReplaceFailed:           "replace_failed",
	GWBypassConstructFailed:         "construct_failed",
}

// String is the stable snake_case label used in gateway_hard_bypass{reason}.
func (r GWBypassReason) String() string {
	if s, ok := gwBypassNames[r]; ok {
		return s
	}
	return "unknown"
}

// ReduceError classifies a reducer internal failure.
type ReduceError int

const (
	ReduceErrNone ReduceError = iota
	ReduceErrAllocFailed
	ReduceErrParseFailed
	ReduceErrInternalAssertion
	ReduceErrFormatUnsupported
)

func reduceErrorToBypass(e ReduceError) GWBypassReason {
	switch e {
	case ReduceErrAllocFailed:
		return GWBypassReduceAllocFailed
	case ReduceErrParseFailed:
		return GWBypassReduceParseFailed
	case ReduceErrFormatUnsupported:
		return GWBypassReduceFormatUnsupported
	}
	// An unclassified failure is reported as an internal assertion rather than
	// waved through: an error we cannot name is not an error we can trust.
	return GWBypassReduceInternalAssertion
}

// StructuralCheck reports how many repairs a messages array NEEDED.
//
// INJECTED rather than assumed, so the decision stays testable against a check
// that is made to fail. MessageHistoryRepair in repair.go is the production
// implementation and is what the reduce stage passes.
//
// The C still has its own message_history_repair in src/server/agent_bridge.c,
// where it is a shared MESSAGE-PIPELINE utility (format detection plus
// per-provider orphan repair) used well beyond this seam. Two implementations
// therefore exist for as long as the cutover runs; that is deliberate and
// temporary, and the Go port is pinned against the C's cases so they cannot
// drift apart silently.
//
// Any NON-ZERO result means the reduced view was not structurally clean.
type StructuralCheck func(messages *JSONValue) int

// GWSnapshotMessages deep-copies a messages array so restoring the pristine
// original is independent of any retained reference.
func GWSnapshotMessages(messages *JSONValue) *JSONValue {
	if messages == nil {
		return nil
	}
	return messages.Clone()
}

// GWSnapshotTokenCount is the chars/4 estimate of a messages array.
func GWSnapshotTokenCount(messages *JSONValue) int {
	return nodeTokenEstimate(messages)
}

// GWShouldApply decides whether the reduced payload may be dispatched.
//
// repair may be nil, in which case the structural check is SKIPPED and the
// caller is trusted to have verified the view another way. Passing nil when you
// have not is how an orphaned tool pair reaches a provider.
func GWShouldApply(reduceOK bool, res *ReduceResult, err ReduceError, repair StructuralCheck) GWBypassReason {
	if !reduceOK || res == nil {
		if res == nil {
			return reduceErrorToBypass(ReduceErrInternalAssertion)
		}
		return reduceErrorToBypass(err)
	}

	// A genuine, APPLIED reduction is the only thing worth mutating for.
	// Measure-only, already-reduced, skipped and no-op all land here.
	if res.Messages == nil || !res.Mutated || res.Reason != ReduceReasonReduced {
		return GWBypassNoOp
	}

	// A reduction that did not actually shrink is not worth the blast radius.
	if res.ReducedTokens >= res.BaselineTokens {
		return GWBypassNoOp
	}

	// Defence in depth: the reduced view must contain no orphaned
	// tool_use/tool_result pair. Run the check on a COPY so the result is never
	// mutated; any repair the copy needed means the view was already broken.
	if repair != nil {
		probe := res.Messages.Clone()
		if probe == nil {
			return GWBypassSnapshotOOM // cannot verify -> never send unverifiable
		}
		// NON-ZERO, not just positive: a negative error code also means the view
		// is not provably clean.
		if repair(probe) != 0 {
			return GWBypassStructuralViolation
		}
	}
	return GWBypassNone
}

// GWReplaceMessages installs the reduced array under key.
//
// On failure the container is left byte-intact and `reduced` stays caller-owned,
// so a failed replace can always fall back to forwarding the pristine request.
func GWReplaceMessages(container *JSONValue, key string, reduced *JSONValue) bool {
	if container == nil || container.Kind != JSONObject || key == "" || reduced == nil {
		return false
	}
	container.Set(key, reduced)
	return true
}

// GWProvenanceMarkReduced records that a seam reduced this request, so a later
// seam re-measures but does not re-reduce.
func GWProvenanceMarkReduced(st *ReduceState) {
	if st != nil {
		st.Reduced = true
	}
}

// GWProvenanceClear resets the per-request provenance flag.
func GWProvenanceClear(st *ReduceState) {
	if st != nil {
		st.Reduced = false
	}
}
