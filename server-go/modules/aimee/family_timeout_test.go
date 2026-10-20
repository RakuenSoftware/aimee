package aimee

import (
	"context"
	"github.com/JBailes/aimee/server-go/bus"
	"testing"
	"time"
)

func TestOperationBudgetPreservesOrdinaryAndExpiredDeadlines(t *testing.T) {
	for _, budget := range []time.Duration{0, CanonicalHistoryTimeout} {
		called := false
		f := Family{Name: "budget", Stage: 1, Ops: map[uint32]Op{1: {Name: "probe", Args: 0, Timeout: budget, Run: func(ctx context.Context, _ Queryer, _ []string) (uint32, []string, error) {
			called = true
			until, ok := ctx.Deadline()
			if !ok {
				t.Fatal("operation lost deadline")
			}
			expected := budget
			if expected == 0 {
				expected = DefaultTimeout
			}
			remaining := time.Until(until)
			if remaining > expected || remaining < expected-time.Second {
				t.Fatalf("budget=%s remaining=%s", budget, remaining)
			}
			return StatusOK, nil, nil
		}}}}
		request := []byte{1, 0, 0, 0, 0, 0, 0, 0}
		_, status := f.Handler(&fakeDB{})(bus.ModuleInvocation{StageID: 1}, request)
		if status != bus.ModuleStatusOK || !called {
			t.Fatal(status, called)
		}
		called = false
		_, status = f.Handler(&fakeDB{})(bus.ModuleInvocation{StageID: 1, DeadlineNS: 1}, request)
		if status != bus.ModuleStatusCancelled || called {
			t.Fatal("expired caller deadline extended", status, called)
		}
	}
}
