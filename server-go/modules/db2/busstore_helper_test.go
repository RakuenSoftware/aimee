package db2

import (
	"context"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	modulepg "github.com/JBailes/aimee/server-go/modules/postgres"
	storage "github.com/JBailes/aimee/server-go/postgres"
)

// liveBusBackedStore builds a Store that reaches the database through the
// postgres module's handler, for suites that want to prove the operations
// behave the same over the wire as they do against a pool.
//
// It binds the handler directly rather than going through the event bus. The
// bus is proven elsewhere; what is in question here is whether the operations'
// values survive the codec, and putting a transport in between would mean a
// failure could be either.
func liveBusBackedStore(t *testing.T) (Store, func()) {
	t.Helper()
	handler := modulepg.NewSQLHandler()
	invocation := bus.ModuleInvocation{
		StageID: modulepg.StageSQL, PrincipalRef: 29, SrcHandle: 1}
	client := storage.New(func(ctx context.Context, body []byte) ([]byte, error) {
		reply, status := handler.Handle(invocation, body)
		if status != bus.ModuleStatusOK {
			t.Fatalf("postgres module status = %v", status)
		}
		return reply, nil
	})
	return NewBusStore(client).ForOperation("db2_live"), func() {
		modulepg.Close()
	}
}
