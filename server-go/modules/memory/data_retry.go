package memory

import (
	"context"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

// A deadlock aborts the PostgreSQL transaction. A deletion can then start from
// its original scope and authority in a fresh transaction, including admission
// and audit, rather than reporting a healthy memory owner as unavailable.
// Caller-owned transactions and other failures retain their existing contract.
func handleData(options handlerOptions, invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	request, err := decodeDataRequest(body)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	backend, ok := options.data.(*postgresDataStore)
	retry := ok && options.placement == PlacementKB &&
		(request.Operation == "delete-as" || request.Operation == "delete")
	if retry {
		_, retry = backend.db.(store.DB)
	}
	if !retry {
		return handleDataAttempt(options, invocation, request, nil)
	}

	parent := options.dataContext
	if parent == nil {
		parent = context.Background()
	}
	ctx, cancel := context.WithTimeout(parent, invocation.Remaining(dataTimeout))
	defer cancel()
	options.dataContext = ctx
	for attempt := 0; ; attempt++ {
		var failure error
		result, status := handleDataAttempt(options, invocation, request, &failure)
		if status != bus.ModuleStatusInternal || memoryFailureClass(failure) != "sqlstate_40P01" || attempt == 2 {
			return result, status
		}
		// handleDataAttempt has rolled back and discarded its audit batch before
		// returning. Bound both retries by the original request's deadline.
		timer := time.NewTimer(time.Duration(attempt+1) * 10 * time.Millisecond)
		select {
		case <-ctx.Done():
			timer.Stop()
			return nil, bus.ModuleStatusCancelled
		case <-timer.C:
		}
	}
}
