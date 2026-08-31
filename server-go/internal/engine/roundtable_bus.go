package engine

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"sync"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	roundtablemod "github.com/JBailes/aimee/server-go/modules/roundtable"
	roundtablecfg "github.com/JBailes/aimee/server-go/modules/roundtable/panel"
)

// Bus identity for this process, matching its generated grant. The contract
// validator keeps the ref above every module ref and pins the kind to a stage
// some module actually serves.
const (
	BusPrincipalClass  uint32 = 1
	WFEBusPrincipalRef uint32 = 64
)

// BusReviewer reaches the roundtable module over the event bus.
//
// The panel is one implementation in one place; this is what stops the control
// plane from being a second host for it. The module convenes the seats and
// spends the money, and the reply arrives correlated on the same bus -- there
// is no polling and no second transport.
type BusReviewer struct {
	// mu serializes reviews. The WFE's shared ConcurrentModuleCaller still
	// multiplexes this call with DB/config/delegate traffic on its one admitted
	// principal.
	mu      sync.Mutex
	caller  busStageCaller
	timeout time.Duration
}

type busStageCaller interface {
	Call(context.Context, uint32, uint32, uint64, time.Duration, []byte) ([]byte, error)
}

// NewBusReviewer attaches to the daemon's module bus as the WFE principal.
func NewBusReviewer(ctx context.Context, socketPath string, principalClass, principalRef uint32,
	timeout time.Duration) (*BusReviewer, error) {
	client, err := bus.ConnectClient(ctx, socketPath, principalClass, principalRef)
	if err != nil {
		return nil, fmt.Errorf("attach to the module bus: %w", err)
	}
	caller, err := bus.NewModuleCaller(client)
	if err != nil {
		return nil, err
	}
	return NewBusReviewerFromCaller(caller, timeout)
}

// NewBusReviewerFromCaller uses an already attached caller. Long-lived
// processes must prefer this constructor: the bus admits one live slot per
// principal, so attaching again with the same identity is correctly denied.
func NewBusReviewerFromCaller(caller busStageCaller, timeout time.Duration) (*BusReviewer, error) {
	if caller == nil {
		return nil, errors.New("roundtable bus caller is not configured")
	}
	if timeout <= 0 {
		// A review runs a panel of live agents; the module enforces its own
		// per-panel deadline, and this is only the backstop for a module that
		// never answers at all.
		timeout = 20 * time.Minute
	}
	return &BusReviewer{caller: caller, timeout: timeout}, nil
}

// Review sends one review to the module and returns its verdict.
func (r *BusReviewer) Review(ctx context.Context, request roundtablecfg.ReviewRequest) (roundtablecfg.RunResult, error) {
	if r == nil || r.caller == nil {
		return roundtablecfg.RunResult{}, errors.New("roundtable bus reviewer is not configured")
	}
	body, err := json.Marshal(request)
	if err != nil {
		return roundtablecfg.RunResult{}, err
	}
	r.mu.Lock()
	defer r.mu.Unlock()
	reply, err := r.caller.Call(ctx, roundtablemod.EventReview, roundtablemod.StageReview, 0, r.timeout, body)
	if err != nil {
		return roundtablecfg.RunResult{}, fmt.Errorf("roundtable review over the bus: %w", err)
	}
	var result roundtablecfg.RunResult
	if err := json.Unmarshal(reply, &result); err != nil {
		return roundtablecfg.RunResult{}, fmt.Errorf("decode roundtable result: %w", err)
	}
	return result, nil
}
