package postgres

import (
	"context"
	"errors"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/db"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"github.com/jackc/pgx/v5/pgxpool"
)

type handlerCaller func(ring.ModuleInvocation, []byte) ([]byte, ring.ModuleStatus)

func (h handlerCaller) Call(_ context.Context, _, stage uint32, _ uint64, _ time.Duration,
	request []byte) ([]byte, error) {
	reply, status := h(ring.ModuleInvocation{StageID: stage}, request)
	if status != ring.ModuleStatusOK {
		return nil, errors.New("module refused at the transport")
	}
	return reply, nil
}

// The SQL stage registers before its database is reachable. What it answers in
// that window must read, through the real client, as the store not being
// available yet -- which the store module's startup waits out -- and must keep
// the stage's reason.
func TestNoDatabaseRefusalReadsAsUnavailable(t *testing.T) {
	noDatabase := func(context.Context) (*pgxpool.Pool, error) {
		return nil, errors.New("AIMEE_STORE_URL is unset")
	}
	handler := (&sqlHandler{txs: newTxRegistry(), poolFn: noDatabase, migrationPoolFn: noDatabase}).handle
	store, err := db.NewStore(handlerCaller(handler))
	if err != nil {
		t.Fatal(err)
	}
	_, _, err = store.CurrentSchemaVersion(context.Background(), "aimee")
	var refused *db.StoreError
	if !errors.Is(err, db.ErrStoreUnavailable) || !errors.As(err, &refused) ||
		refused.SQLState != sqlStateUnableToConnect {
		t.Fatalf("no-database refusal = %v, want ErrStoreUnavailable carrying SQLSTATE %s",
			err, sqlStateUnableToConnect)
	}
}
