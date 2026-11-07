package families

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"testing"
)

type measuredHistoryBatch struct {
	*originalHistoryFixture
	locks, existing, inserts, fences int
	foreign, fenced, failInsert      bool
}

func newMeasuredHistoryBatch() *measuredHistoryBatch {
	return &measuredHistoryBatch{originalHistoryFixture: &originalHistoryFixture{sessDB: newSessDB()}}
}
func (d *measuredHistoryBatch) QueryRow(ctx context.Context, sql string, args ...any) store.Row {
	if sql == sessionIRLockSQL {
		d.locks++
		if d.foreign {
			return sessRow{db: &sessDB{}, values: []any{"other"}}
		}
	}
	if sql == nativeRecoveryFenceSQL {
		d.fences++
		if d.fenced {
			return sessRow{db: &sessDB{}, values: []any{1}}
		}
	}
	return d.originalHistoryFixture.QueryRow(ctx, sql, args...)
}
func (d *measuredHistoryBatch) Query(ctx context.Context, sql string, args ...any) (store.Rows, error) {
	if sql == sessionIRHistoryExistingBatchSQL {
		d.existing++
	}
	if sql == sessionIRHistoryInsertBatchSQL {
		d.inserts++
		if d.failInsert {
			return nil, errors.New("controlled insert failure")
		}
	}
	return d.originalHistoryFixture.Query(ctx, sql, args...)
}
func batchHistoryMessages(t *testing.T, n int) string {
	t.Helper()
	rows := make([]json.RawMessage, n)
	for i := range rows {
		rows[i] = json.RawMessage(fmt.Sprintf(`{"role":"user","blocks":[{"type":"text","text":"batch message %d"}]}`, i))
	}
	out, err := json.Marshal(rows)
	if err != nil {
		t.Fatal(err)
	}
	return string(out)
}
func TestInputHistoryBatchBoundsRoundTripsExactReplayAndConflict(t *testing.T) {
	d := newMeasuredHistoryBatch()
	fields := []string{"s", "checkpoint", "primary", "actor", batchHistoryMessages(t, 8)}
	status, _, err := sessionIRHistorySaveInputAuthorized(context.Background(), d, fields, true, true)
	if err != nil || status != store.StatusOK || len(d.events) != 9 {
		t.Fatalf("initial batch failed: status=%d err=%v rows=%d", status, err, len(d.events))
	}
	if d.locks != 1 || d.existing != 1 || d.inserts != 1 || d.fences != 1 {
		t.Fatalf("checkpoint performed per-event RPCs: %d/%d/%d/%d", d.locks, d.existing, d.inserts, d.fences)
	}
	d.fenced = true
	status, _, err = sessionIRHistorySaveInputAuthorized(context.Background(), d, fields, true, true)
	if err != nil || status != store.StatusOK || len(d.events) != 9 || d.inserts != 1 || d.fences != 1 {
		t.Fatal("exact replay changed rows or consulted a later fence")
	}
	fields[4] = batchHistoryMessages(t, 7)
	status, _, err = sessionIRHistorySaveInputAuthorized(context.Background(), d, fields, true, true)
	if err != nil || status != store.StatusInvalid || len(d.events) != 9 || d.inserts != 1 {
		t.Fatal("conflicting checkpoint changed prior events")
	}
}
func TestInputHistoryBatchValidatesWholePayloadOwnerAndRecoveryBeforeMutation(t *testing.T) {
	for _, mode := range []string{"malformed", "owner", "fence", "insert"} {
		t.Run(mode, func(t *testing.T) {
			d := newMeasuredHistoryBatch()
			fields := []string{"s", "checkpoint", "primary", "actor", batchHistoryMessages(t, 8)}
			switch mode {
			case "malformed":
				fields[4] = `[{"role":"user","blocks":[{"type":"text","text":"first"}]},{"role":"","blocks":[]}]`
			case "owner":
				d.foreign = true
			case "fence":
				d.fenced = true
			case "insert":
				d.failInsert = true
			}
			status, _, err := sessionIRHistorySaveInputAuthorized(context.Background(), d, fields, true, true)
			if (err == nil && status == store.StatusOK) || len(d.events) != 0 {
				t.Fatal("failure admitted partial checkpoint")
			}
			if mode == "insert" && err == nil {
				t.Fatal("insert outage became rejection without error")
			}
			if mode == "malformed" && (d.locks != 0 || d.existing != 0 || d.inserts != 0) {
				t.Fatal("invalid replacement reached store")
			}
		})
	}
}
