package panel

import "context"

// Run is the identity and budget a review executes under. The panel needs these
// to key durable delegate slots, bound spend and reach the worktree; it needs
// nothing else about the workflow item that may own the review.
type Run struct {
	// ID is the review identity. It is echoed by every seat and checked, so a
	// report about a different run is detectable.
	ID string
	// Stage names the workflow node, or is empty for a standalone review. It
	// participates in the durable delegate slot, never in the prompt.
	Stage string
	// ExecutionVersion separates one attempt of a step from the next, so a retry
	// does not replay the previous attempt's seats.
	ExecutionVersion string
	Workdir          string
	// ReplayOnly forbids launching fresh delegate work: the spend was already
	// reconciled, so only an existing durable result may be consumed.
	ReplayOnly bool
	// CostLimitUSD is the whole review's ceiling, zero meaning unbounded. The
	// panel subdivides it across concurrent seats itself.
	CostLimitUSD float64
	// OriginalRequest is what the artifact is judged against. Alignment to it is
	// the first question every seat answers, so a review without it is not a
	// weaker review -- it is a different one.
	OriginalRequest string
	// Reviewed is the exact artifact under review.
	Reviewed Artifact
}

// Artifact is the immutable content a review is about. Hash is its identity:
// every seat echoes it back, so a verdict can be tied to the bytes it was made
// about rather than merely asserted about them.
type Artifact struct {
	Stage   string
	Content string
	Hash    string
}

// SeatRequest is one reviewing agent's turn. The panel describes the seat it
// wants; which agent serves it, and how, belongs entirely to the transport.
type SeatRequest struct {
	// Role is the delegate role a seat is admitted under. The roundtable reviews,
	// so this is always the review role -- it is stated rather than assumed
	// because admission and routing both key on it.
	Role    string
	Persona string
	// Selector is an operator's positive pin. Empty means ordinary eligibility
	// routing, which is the normal case -- it is never an exclusion list.
	Selector string
	// Participant continues an existing seat rather than opening a new one, and
	// is opaque: the panel returns it without interpreting it.
	Participant string
	Prompt      string
	// DurableSlot distinguishes concurrent seats that would otherwise look
	// identical, so two capacity seats cannot collapse onto one remote result.
	DurableSlot   string
	ArtifactStage string
	ArtifactHash  string
	MaxCostUSD    float64
	// Tools keeps the seat on the tool-capable transport. It is not an
	// optimisation to drop: a CLI-backed agent has no HTTP request URL, and
	// sending its continuation down the simple path silently breaks the seat.
	Tools bool
	// MaxTurnsCap bounds a seat without overriding a smaller role or agent cap.
	MaxTurnsCap int
}

// SeatResult is what a seat returned, or why it did not.
type SeatResult struct {
	Participant string
	Response    string
	CostUSD     float64
	// CostUnknown means no measurement was recorded, so CostUSD is a lower bound
	// rather than actual spend. It must never be committed as a measured zero.
	CostUnknown bool
	Err         error
	// FailureCategory names why the seat failed, in the transport's own terms
	// ("deadline", "capacity_backpressure", "replay_unavailable", ...). The panel
	// reports it verbatim and never parses Err: how a delegate fails is the
	// transport's knowledge, and duplicating that taxonomy here would leave two
	// copies to drift apart.
	FailureCategory string
	// FailureDetail is the human-readable reason, already credential-redacted by
	// the transport. The panel stores it verbatim: redaction is a property of the
	// diagnostics the transport produces, and a second copy of that table here
	// would be one more thing to keep in step.
	FailureDetail string
	// ReplayLost marks a seat whose durable result is gone. Retrying cannot fix
	// that, so the caller must reach reservation recovery rather than park.
	ReplayLost bool
}

// Delegates is the resource plane a panel convenes over.
//
// Group is not a convenience over One: seats must run concurrently and share
// one cost reservation, and a plane that cannot do that cannot host a
// roundtable at all. The panel never reconstructs grouping or participant
// identity from single calls.
type Delegates interface {
	Group(ctx context.Context, run Run, requests []SeatRequest) []SeatResult
	One(ctx context.Context, run Run, request SeatRequest) SeatResult
}
