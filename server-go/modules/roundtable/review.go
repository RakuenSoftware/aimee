package roundtable

import (
	"context"
	"encoding/json"

	"github.com/JBailes/aimee/server-go/bus"
	roundtablecfg "github.com/JBailes/aimee/server-go/internal/roundtable"
)

// The review stage carried over the event bus.
//
// StageDeliberate above is a pure rubric with a 40-byte fixed contract, which is
// why it migrated with the other modules. A review is not that: it needs the
// engine -- delegates, artifact store, worktrees, preset store, cost accounting
// -- so it stayed on a bespoke AF_UNIX HTTP proxy (src/server/wfe_roundtable_proxy.c)
// while everything around it moved to the bus.
//
// That proxy is a second transport doing what the bus already does, with its own
// framing, timeouts and failure taxonomy. Its cost is not theoretical: the C side
// and the Go side ended up with separate notions of the same panel settings,
// reconciled nowhere, so a chair-synthesis guard added in C had no effect on
// reviews at all.
//
// The handler is a closure over the reviewer rather than a package-level
// function, so the process that already owns the engine serves the stage. The
// module runtime owns attach, deadlines, cancellation and correlation; this only
// decodes, calls, and encodes.
const (
	// EventReview is the next free kind after the highest allocated module kind.
	EventReview uint32 = 10753
	StageReview uint32 = 2
)

// Reviewer is the engine capability this stage needs. Narrow on purpose: the
// stage depends on the one method, not on the runner.
type Reviewer interface {
	Review(context.Context, roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error)
}

// NewReviewHandler adapts a Reviewer to the module contract.
//
// Body in and out is the SAME JSON as the HTTP route carried, so the wire
// contract does not change with the transport -- only the transport does. A
// 16 MiB artifact fits: ModuleMessageMaxBody and MaxArtifactBytes are both
// 16 MiB, which is why this can move to the bus at all.
func NewReviewHandler(reviewer Reviewer) bus.ModuleHandler {
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		if reviewer == nil {
			return nil, bus.ModuleStatusInternal
		}
		if invocation.StageID != StageReview {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var decoded roundtablecfg.ReviewRequest
		if err := json.Unmarshal(request, &decoded); err != nil {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if invocation.Cancelled() {
			return nil, bus.ModuleStatusCancelled
		}
		// The runtime re-checks cancellation and the deadline before publishing,
		// so a review that overruns is reported as such rather than replied to.
		result, err := reviewer.Review(context.Background(), decoded)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		body, err := json.Marshal(result)
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		if uint32(len(body)) > bus.ModuleMessageMaxBody {
			return nil, bus.ModuleStatusInternal
		}
		return body, bus.ModuleStatusOK
	}
}
