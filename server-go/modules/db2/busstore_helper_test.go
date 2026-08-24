package db2

import (
	"context"
	"os"
	"sync/atomic"
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
// wireCalls counts every request that actually crossed the storage codec.
//
// Without it, a parity run proves nothing it claims: the operations pass
// identically whether they reached PostgreSQL through the module or through a
// pool, so a run where AIMEE_DB2_STORE never arrived reports the same green as
// one where the comparison happened.
var wireCalls atomic.Int64

func liveBusBackedStore(t *testing.T) (Store, func()) {
	t.Helper()
	handler := modulepg.NewSQLHandler()
	invocation := bus.ModuleInvocation{
		StageID: modulepg.StageSQL, PrincipalRef: 29, SrcHandle: 1}
	client := storage.New(func(ctx context.Context, body []byte) ([]byte, error) {
		wireCalls.Add(1)
		reply, status := handler(invocation, body)
		if status != bus.ModuleStatusOK {
			t.Fatalf("postgres module status = %v", status)
		}
		return reply, nil
	})
	return NewBusStore(client).ForOperation("db2_live"), func() {
		modulepg.Close()
	}
}

func TestTheParityRunActuallyUsedTheWire(t *testing.T) {
	// The guard for the failure a typo check cannot reach: the variable spelled
	// correctly and still not arriving, or a future edit that routes bus mode
	// back through a pool for some reason that looks good at the time.
	//
	// Asserting "the store is a *BusStore" would be circular -- it would assert
	// the branch this test took. Counting calls through the codec asserts that
	// bytes were encoded and decoded, which is the thing being claimed.
	if os.Getenv("AIMEE_DB2_STORE") != "bus" {
		t.Skip("not a bus-mode run; there is no wire to have used")
	}
	if os.Getenv("AIMEE_DB2_URL") == "" {
		t.Skip("AIMEE_DB2_URL is unset; this test needs a real database")
	}
	before := wireCalls.Load()
	store, done := liveStore(t)
	defer done()
	rows, err := store.Query(context.Background(), "SELECT 1")
	if err != nil {
		t.Fatalf("a statement over the wire failed: %v", err)
	}
	rows.Close()
	if after := wireCalls.Load(); after <= before {
		t.Fatalf("the store answered without crossing the codec (%d calls before, %d after); "+
			"a parity run in this state compares the pool with itself", before, after)
	}
}
