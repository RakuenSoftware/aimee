package engine

import "github.com/JBailes/aimee/server-go/delegate"

// Delegate execution crosses the module bus through the repository-level
// delegate contract. These aliases keep the workflow engine API stable without
// making one Go module import another.
type (
	DelegateRequest     = delegate.DelegateRequest
	DelegateResult      = delegate.DelegateResult
	DelegateGroupResult = delegate.DelegateGroupResult
	AgentClient         = delegate.AgentClient
	DelegateGroupClient = delegate.DelegateGroupClient
	// DelegateExecutionError carries the billing boundary across transport and
	// runner layers, so both sides must agree on the same type.
	DelegateExecutionError = delegate.DelegateExecutionError
)

var (
	ErrDelegateUnassignedExpired    = delegate.ErrDelegateUnassignedExpired
	ErrDelegateCancelUnacknowledged = delegate.ErrDelegateCancelUnacknowledged
	ErrDelegateTerminal             = delegate.ErrDelegateTerminal
	ErrDelegateReplayUnavailable    = delegate.ErrDelegateReplayUnavailable
	ErrDelegateCapacityDeadline     = delegate.ErrDelegateCapacityDeadline

	safeDiagnostic         = delegate.SafeDiagnostic
	isCapacityBackpressure = delegate.IsCapacityBackpressure
)
