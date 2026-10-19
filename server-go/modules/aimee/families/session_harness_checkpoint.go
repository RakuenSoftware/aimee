package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Compare-and-swap native cache metadata under the existing session append lock.
// An empty expected value claims a missing handle. A losing or stale claimant
// cannot overwrite another execution's invalidation/completion checkpoint.
const sessionHarnessCompareSwapSQL = `INSERT INTO session_harness_handles(session_id,harness,native_id)
 SELECT $1,$2,$4 WHERE
 ($3='' AND NOT EXISTS(SELECT 1 FROM session_harness_handles WHERE session_id=$1 AND harness=$2))
 OR EXISTS(SELECT 1 FROM session_harness_handles WHERE session_id=$1 AND harness=$2 AND native_id=$3)
 ON CONFLICT(session_id,harness) DO UPDATE SET native_id=excluded.native_id
 WHERE session_harness_handles.native_id=$3`

func sessionHarnessCompareSwap(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if !validHarnessIdentity([]string{f[0], f[1], f[3], f[4]}) || len(f[2]) > 1024 {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	if err := q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&owner); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if owner != f[4] {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, sessionHarnessCompareSwapSQL, f[0], f[1], f[2], f[3])
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusMissing, nil, nil
	}
	return store.StatusOK, nil, nil
}
