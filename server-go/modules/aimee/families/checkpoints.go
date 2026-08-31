package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Session checkpoints: a labelled snapshot per session, listed newest-first.
const (
	EventCheckpoints uint32 = 11790
	StageCheckpoints uint32 = 14

	opCheckpointInsert uint32 = 1
	opCheckpointGet    uint32 = 2
	opCheckpointList   uint32 = 3
	opCheckpointDelete uint32 = 4
)

// checkpointListMax is the ceiling the client already enforces; the module
// re-checks it because a module may not assume its peer is the shipped client.
const checkpointListMax = 64

// checkpointColumns is the row every checkpoint operation answers with.
//
// created_at is formatted at the boundary rather than stored as text: the
// column is TIMESTAMPTZ, and this is the spelling SQLite's datetime('now')
// produced, which is what the wire has always carried.
const checkpointColumns = `id, task_id, session_id, label, snapshot,
	                       to_char(created_at AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS')`

const (
	checkpointInsertSQL = `INSERT INTO checkpoints (task_id, session_id, label, snapshot)
	                            VALUES ($1, $2, $3, $4)
	                         RETURNING ` + checkpointColumns

	checkpointGetSQL = `SELECT ` + checkpointColumns + ` FROM checkpoints WHERE id = $1`

	// id breaks the tie on created_at. The C ordered by created_at alone, so
	// two checkpoints taken in the same second came back in whatever order the
	// scan produced -- and a LIMIT over that is not a stable page.
	checkpointListSQL = `SELECT ` + checkpointColumns + `
	                       FROM checkpoints
	                      ORDER BY created_at DESC, id DESC
	                      LIMIT $1`

	checkpointDeleteSQL = `DELETE FROM checkpoints WHERE id = $1`
)

// checkpointRow reads one row into the six cells the wire carries.
func checkpointRow(scan func(...any) error) ([]string, error) {
	var (
		id, taskID                            int64
		sessionID, label, snapshot, createdAt string
	)
	if err := scan(&id, &taskID, &sessionID, &label, &snapshot, &createdAt); err != nil {
		return nil, err
	}
	return []string{
		store.I64toa(id), store.I64toa(taskID),
		sessionID, label, snapshot, createdAt,
	}, nil
}

// checkpointInsert is op 1. RETURNING gives back the stored row in one
// statement -- the C inserted, then read last_insert_rowid(), then SELECTed it
// back, which is three statements and a rowid that is only correct because
// nothing else was writing on that connection.
func checkpointInsert(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	taskID, ok := store.Atoi64(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	if f[0] == "" {
		// The C refused a NULL label; the column is NOT NULL, and an empty
		// label is a checkpoint nothing can later be found by.
		return store.StatusInvalid, nil, nil
	}
	cells, err := checkpointRow(q.QueryRow(ctx, checkpointInsertSQL, taskID, f[1], f[0], f[3]).Scan)
	if err != nil {
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// checkpointGet is op 2.
//
// A missing checkpoint is MISSING. The generated C stage could not produce that
// status here -- its row-returning branch mapped every non-zero return to
// FAILED -- but the catalog declares it and the C client maps any non-OK status
// to the same -1, so saying the truthful thing changes nothing for a caller.
func checkpointGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	cells, err := checkpointRow(q.QueryRow(ctx, checkpointGetSQL, id).Scan)
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	return store.StatusOK, cells, nil
}

// checkpointList is op 3: the most recent checkpoints, flattened.
//
// A list reply carries no row count -- the caller divides by the row width -- so
// this must emit a whole number of six-cell rows or none.
func checkpointList(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	limit, okLimit := store.Atoi(f[0])
	max, okMax := store.Atoi(f[1])
	if !okLimit || !okMax || max <= 0 || max > checkpointListMax {
		return store.StatusInvalid, nil, nil
	}
	// The C read `limit > 0 ? limit : max` and then truncated to max, so a
	// limit above max was silently capped rather than refused.
	if limit <= 0 || limit > max {
		limit = max
	}

	rows, err := q.Query(ctx, checkpointListSQL, limit)
	if err != nil {
		return 0, nil, err
	}
	defer rows.Close()

	cells := make([]string, 0, limit*6)
	for rows.Next() {
		row, err := checkpointRow(rows.Scan)
		if err != nil {
			return 0, nil, err
		}
		cells = append(cells, row...)
	}
	if err := rows.Err(); err != nil {
		// Reported rather than returning the rows read so far: a partial list
		// is indistinguishable from a complete short one on this wire.
		return 0, nil, err
	}
	// An empty list is OK with no cells, not MISSING: the caller asked what was
	// there and the answer was nothing.
	return store.StatusOK, cells, nil
}

// checkpointDelete is op 4. Deleting a checkpoint that is not there is a
// failure, as it was in the C: the caller asked for a specific id, and nothing
// having happened is not the outcome it asked for.
func checkpointDelete(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	id, ok := store.Atoi64(f[0])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	tag, err := q.Exec(ctx, checkpointDeleteSQL, id)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() == 0 {
		return store.StatusFailed, nil, nil
	}
	return store.StatusOK, nil, nil
}

// Checkpoints is the family, ready to be bound to kind 11790.
var Checkpoints = store.Family{
	Name:  "checkpoints",
	Event: EventCheckpoints,
	Stage: StageCheckpoints,
	Ops: map[uint32]store.Op{
		opCheckpointInsert: {Name: "checkpoint_insert", Cells: 6, Args: 4, Tx: true, Run: checkpointInsert},
		opCheckpointGet:    {Name: "checkpoint_get", Cells: 6, Args: 1, Run: checkpointGet},
		opCheckpointList:   {Name: "checkpoint_list", Cells: 6, Args: 2, Run: checkpointList},
		opCheckpointDelete: {Name: "checkpoint_delete", Args: 1, Tx: true, Run: checkpointDelete},
	},
}
