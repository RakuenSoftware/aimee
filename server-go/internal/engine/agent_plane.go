package engine

import (
	"context"
	"errors"

	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/JBailes/aimee/server-go/modules/delegates/plane"
)

// The delegate resource-plane client lives in modules/delegates/plane so the
// roundtable module process can reach the same plane without importing this
// package. These are aliases, not wrappers: one implementation, and no
// conversion at a boundary that both sides would otherwise have to maintain.
type (
	DelegateRequest     = plane.DelegateRequest
	DelegateResult      = plane.DelegateResult
	DelegateGroupResult = plane.DelegateGroupResult
	AgentClient         = plane.AgentClient
	DelegateGroupClient = plane.DelegateGroupClient
	HTTPAgentClient     = plane.HTTPAgentClient
	AgentHTTPConfig     = plane.AgentHTTPConfig
	// DelegateExecutionError carries the billing boundary across transport and
	// runner layers, so both sides must agree on the same type.
	DelegateExecutionError = plane.DelegateExecutionError
)

var (
	NewHTTPAgentClient = plane.NewHTTPAgentClient

	ErrDelegateUnassignedExpired    = plane.ErrDelegateUnassignedExpired
	ErrDelegateCancelUnacknowledged = plane.ErrDelegateCancelUnacknowledged
	ErrDelegateNoJobID              = plane.ErrDelegateNoJobID
	ErrDelegateTerminal             = plane.ErrDelegateTerminal
	ErrDelegateReplayUnavailable    = plane.ErrDelegateReplayUnavailable

	safeDiagnostic                = plane.SafeDiagnostic
	isCapacityBackpressure        = plane.IsCapacityBackpressure
	MinDelegatePendingTimeout     = plane.MinDelegatePendingTimeout
	DefaultDelegatePendingTimeout = plane.DefaultDelegatePendingTimeout
	MaxDelegatePendingTimeout     = plane.MaxDelegatePendingTimeout
)

// TerminalJobCanceller closes the durable commit-to-cancel crash window.
//
// It stays here rather than moving to the plane because it is a scheduler
// concern that reads the Go-owned lifecycle tables to find work items that can
// no longer execute. The plane client deliberately holds no database handle;
// this composes that client with the store instead of reintroducing one.
type TerminalJobCanceller struct {
	store  *db1.Store
	client *plane.HTTPAgentClient
}

func NewTerminalJobCanceller(store *db1.Store, client *plane.HTTPAgentClient) *TerminalJobCanceller {
	return &TerminalJobCanceller{store: store, client: client}
}

// CancelTerminalJobs cancels resource-plane jobs owned by work items that have
// stopped. A reservation is released only after the plane acknowledges the
// cancellation; failures stay mapped and are retried on the next scheduler fill,
// which is what makes a crash between the lifecycle commit and the cancellation
// recoverable rather than a permanently orphaned job.
func (c *TerminalJobCanceller) CancelTerminalJobs(ctx context.Context) (int, error) {
	if c == nil || c.store == nil || c.client == nil {
		return 0, nil
	}
	mappings, err := c.store.TerminalDelegateJobs(ctx)
	if err != nil {
		return 0, err
	}
	cancelled := 0
	var errs []error
	for _, mapping := range mappings {
		if err := c.client.CancelAndRelease(mapping.JobID, mapping.ExecutionKey, ctx); err != nil {
			errs = append(errs, err)
			continue
		}
		cancelled++
	}
	return cancelled, errors.Join(errs...)
}
