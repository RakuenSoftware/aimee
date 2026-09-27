package families

import (
	"context"
	"time"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
	executionpolicy "github.com/JBailes/aimee/server-go/modules/execution-policy"
)

// Runs only through the host-only transactional operation. Session ownership
// comes from the existing session directory, not from the contract being issued.
func sessionExplorationApply(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	principal, sid, body := f[0], f[1], f[2]
	if principal == "" || sid == "" || len(principal) > 128 || len(sid) > 128 || len(body) > 65536 {
		return store.StatusInvalid, nil, nil
	}
	var owner string
	err := q.QueryRow(ctx, `SELECT principal FROM server_sessions WHERE id=$1 FOR UPDATE`, sid).Scan(&owner)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if owner != principal {
		return store.StatusInvalid, nil, nil
	}
	if _, err = q.Exec(ctx, `INSERT INTO session_state(session_id) VALUES($1) ON CONFLICT(session_id) DO NOTHING`, sid); err != nil {
		return 0, nil, err
	}
	var state string
	if err = q.QueryRow(ctx, `SELECT exploration_state FROM session_state WHERE session_id=$1 FOR UPDATE`, sid).Scan(&state); err != nil {
		return 0, nil, err
	}
	next, reply, err := executionpolicy.SessionExploration(principal, sid, []byte(state), []byte(body), time.Now().UTC())
	if err != nil {
		return store.StatusInvalid, nil, nil
	}
	if _, err = q.Exec(ctx, `UPDATE session_state SET exploration_state=$2,updated_at=now() WHERE session_id=$1`, sid, string(next)); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{string(reply)}, nil
}
