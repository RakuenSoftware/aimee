package families

import (
	"context"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const opSessionNativeActiveOwned uint32 = 46

// Discovery precedes model/profile selection. Each result still needs its exact
// physical lease and independently qualified kernel owner before reconciliation.
// Never return a partial list: an omitted abandoned source can block history.
func sessionNativeActiveOwned(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 2 || f[0] == "" || len(f[0]) > 128 || f[1] == "" || len(f[1]) > 128 {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], f[1])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	rows, err := q.Query(ctx, `SELECT harness,binding_sha256,owner_id FROM session_native_reservations WHERE session_id=$1 AND state='active' ORDER BY created_at,owner_id LIMIT 33`, f[1])
	if err != nil {
		return 0, nil, err
	}
	requests := make([]nativeReservationRequest, 0)
	for rows.Next() {
		r := nativeReservationRequest{Version: 1, Session: f[1]}
		if err := rows.Scan(&r.Harness, &r.Binding, &r.Owner); err != nil {
			rows.Close()
			return 0, nil, err
		}
		requests = append(requests, r)
	}
	err = rows.Err()
	rows.Close() // Release the cursor before exact owner lookups on this transaction.
	if err != nil {
		return 0, nil, err
	}
	if len(requests) > 32 {
		return store.StatusInvalid, nil, nil
	}
	owners := make([]json.RawMessage, 0, len(requests))
	bytes := 2
	for _, r := range requests {
		raw, err := json.Marshal(r)
		if err != nil {
			return 0, nil, err
		}
		status, cells, err := sessionNativeReservationLookup(ctx, q, []string{f[0], string(raw)})
		if err != nil {
			return 0, nil, err
		}
		if status != store.StatusOK || len(cells) != 1 {
			return store.StatusInvalid, nil, nil
		}
		bytes += len(cells[0]) + 1
		if bytes > 512*1024 {
			return 0, nil, fmt.Errorf("active native ownership exceeds response bound")
		}
		owners = append(owners, json.RawMessage(cells[0]))
	}
	raw, err := json.Marshal(owners)
	return store.StatusOK, []string{string(raw)}, err
}
