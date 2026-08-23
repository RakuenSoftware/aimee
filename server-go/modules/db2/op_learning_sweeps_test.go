package db2

import (
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func TestAntiPatternBumpDoesNotRequireThePatternToExist(t *testing.T) {
	// Bumping is counting, and counting an occurrence of a pattern nobody has
	// recorded yet is a no-op rather than a failure -- the C reports success
	// whenever the statement ran, and a caller sweeping a batch of observations
	// would otherwise be told its sweep failed because one pattern had been
	// retired underneath it.
	store := &fakeStore{execRowsAt: true, execRows: 0}
	handler := NewDispatchHandler(store)
	request, err := db2contract.EncodeAntiPatternBumpRequest(7)
	if err != nil {
		t.Fatalf("encode: %v", err)
	}
	body, status := handler(invocation(db2contract.StageAntiPatternBump), request)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v for a pattern that is not there", status)
	}
	if decodeErr := db2contract.DecodeAntiPatternBumpReply(body); decodeErr != nil {
		t.Fatalf("decode: %v", decodeErr)
	}
	if store.execCalls != 1 {
		t.Fatalf("statements = %d", store.execCalls)
	}
}
