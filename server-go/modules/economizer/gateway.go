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

// Counter names, kept stable because dashboards key on them.
const (
	StatSessionDisabledBlocks = "session_disabled_blocks"
	StatMutateAttempted       = "mutate_attempted"
	StatMutateApplied         = "mutate_applied"
	Stat4xxRestoreResend      = "4xx_restore_resend"
	Stat5xxDisable            = "5xx_disable"
	StatStreamErrorDisable    = "stream_error_disable"
	StatHardBypass            = "hard_bypass"

	// Reducer state is an optimization, so a store failure degrades silently by
	// design -- the reduction still runs, it just never warms up. That is
	// exactly why it has to be COUNTED: a fold that has quietly stopped reusing
	// its freeze boundary looks like a fold that is working, and the only other
	// evidence is a token bill that does not fall.
	StatStateUnavailable = "state_unavailable"
	StatStateSaveFailed  = "state_save_failed"
)

// PostAction is what the caller must do after the upstream status.
type PostAction int

const (
	PostNone PostAction = iota
	// PostResend means the request was restored to pristine and must be sent
	// once more.
	PostResend
)

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
	GWBypassSessionDisabled     // the breaker is open for this session
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
	GWBypassSessionDisabled:         "session_disabled",
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
// The reduce stage validates canonical tool-call/result structure before the
// reduced history can replace the original. Provider-format conversion belongs
// to the external wire boundary.
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

// GWPostDecision is what a caller must do once the upstream status is known.
//
// It carries no payload and touches no store, so the same decision serves an
// in-process caller that owns the request body and a bus caller that only has
// the status. Restoring the pristine array, tripping the breaker and resending
// are the CALLER's to perform — this only says which of them are owed.
type GWPostDecision struct {
	Action  PostAction
	Restore bool   // put the pristine array back before doing anything else
	Disable bool   // trip the session breaker
	Reason  string // breaker reason, and the label telemetry records
	Counter string // the counter to increment, "" when there is nothing to record
}

// GWPostStatus decides what a buffered turn owes after its upstream status.
//
// 4xx: the provider rejected the payload we built, so the reduction is the prime
// suspect — restore the pristine original, trip the breaker and resend ONCE.
//
// 5xx: provider state is uncertain, since the request may have been partially
// processed, so trip the breaker but do NOT resend. Resending after an ambiguous
// server error risks duplicating work the provider already did.
//
// A turn that was never mutated owes nothing: there is no reduction to blame and
// nothing to restore, so an unrelated provider error must not disable the lever.
func GWPostStatus(httpStatus int, mutated bool) GWPostDecision {
	if !mutated {
		return GWPostDecision{}
	}
	switch httpStatus / 100 {
	case 4:
		return GWPostDecision{
			Action: PostResend, Restore: true, Disable: true,
			Reason: "4xx", Counter: Stat4xxRestoreResend,
		}
	case 5:
		return GWPostDecision{
			Disable: true, Reason: "5xx", Counter: Stat5xxDisable,
		}
	}
	return GWPostDecision{}
}

// GWStreamDisable decides what a STREAMING turn owes when it fails after
// dispatch. Nothing can be restored or resent once bytes have gone to the
// client, so the breaker is all that is left.
//
// Without a session key there is no breaker to trip, and disabling is skipped
// rather than applied to some other identity's session.
func GWStreamDisable(mutated, haveKey bool, reason string) GWPostDecision {
	if !mutated || !haveKey {
		return GWPostDecision{}
	}
	if reason == "" {
		reason = "stream"
	}
	return GWPostDecision{
		Disable: true, Reason: reason, Counter: StatStreamErrorDisable,
	}
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
