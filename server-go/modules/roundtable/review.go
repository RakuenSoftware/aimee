package roundtable

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// The review stage carried over the event bus.
//
// StageDeliberate is a pure rubric with a fixed 40-byte contract, which is why
// it migrated early. A review is not that: it convenes real agents, spends real
// money, and has to reach the delegate resource plane. That is why it stayed on
// a bespoke AF_UNIX HTTP proxy long after everything around it had moved.
//
// It can move now because the panel owns itself: seat fan-out, discussion,
// chairing and the verdict live in modules/roundtable/panel with no
// control-plane imports, and the plane client it convenes over holds no
// database handle. What is left here is only the stage adapter.
const (
	// EventReview is roundtable's second stage kind. The allocation is not a free
	// choice: the process contract fixes it at 4096 + ordinal*256 + stage, and
	// roundtable is ordinal 21, so review is deliberate's successor rather than a
	// kind taken from the top of the range.
	EventReview uint32 = 9474
	StageReview uint32 = 2

	// EventChunkPlan serves budget-sized chunk planning and the synthesis
	// assembly. Same allocation rule: 4096 + 21*256 + stage.
	EventChunkPlan uint32 = 9475
	StageChunkPlan uint32 = 3
)

// Reviewer convenes one roundtable. Narrow on purpose: the stage depends on the
// single capability, not on whatever assembles it.
type Reviewer interface {
	Review(context.Context, panel.ReviewRequest) (panel.RunResult, error)
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
			return []byte("roundtable reviewer is not configured"), bus.ModuleStatusInternal
		}
		// Review and deliberate share a module and are told apart only by stage id,
		// so a deliberate id arriving here is a protocol error, not a review.
		if invocation.StageID != StageReview {
			return nil, bus.ModuleStatusInvalidRequest
		}
		var decoded panel.ReviewRequest
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
			// A request this panel will never accept is reported as invalid, not
			// internal. The distinction is the caller's only way to tell "try
			// again later" from "this will fail every time", and without it a
			// workflow parks and resubmits the same rejected review indefinitely.
			var invalid panel.ValidationError
			if errors.As(err, &invalid) {
				return []byte(invalid.Error()), bus.ModuleStatusInvalidRequest
			}
			// A failed review must never be reported as an empty success: a caller
			// reading an empty result as "approved, no findings" would ship
			// unreviewed work.
			//
			// The reason rides back as the body. A non-OK reply does not carry it
			// over the wire, but the runtime logs it on the way past -- without
			// that, every distinct failure here reaches the caller as the same
			// bare status number and the operator has nothing to go on.
			return []byte(err.Error()), bus.ModuleStatusInternal
		}
		body, err := json.Marshal(result)
		if err != nil {
			return []byte("encode roundtable result: " + err.Error()), bus.ModuleStatusInternal
		}
		if uint32(len(body)) > bus.ModuleMessageMaxBody {
			return []byte("roundtable result exceeds the module message limit"), bus.ModuleStatusInternal
		}
		return body, bus.ModuleStatusOK
	}
}

// NewHandler dispatches the module's stages.
//
// One process serves both, and only the stage id tells them apart, so the split
// is made once here rather than in each handler. A nil reviewer means the review
// stage was never configured; it is rejected rather than silently answered,
// because a review that did not happen must not look like one that found
// nothing.
func NewHandler(reviewer Reviewer) bus.ModuleHandler {
	review := NewReviewHandler(reviewer)
	return func(invocation bus.ModuleInvocation, request []byte) ([]byte, bus.ModuleStatus) {
		switch invocation.StageID {
		case StageDeliberate:
			return Handle(invocation, request)
		case StageReview:
			return review(invocation, request)
		case StageChunkPlan:
			return handleChunkPlan(invocation, request)
		default:
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
}
