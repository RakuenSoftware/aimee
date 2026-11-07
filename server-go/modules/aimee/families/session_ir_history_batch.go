package families

import (
	"context"
	"encoding/json"
	"fmt"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
)

// Each batch is bounded below the SQL module's wire limit. All chunks remain
// inside the existing enclosing transaction and session owner row lock.
const historyAppendBatchBytes = 1 << 20
const historyAppendBatchCount = 128

type historyAppendEntry struct {
	ID      string `json:"event_id"`
	Payload string `json:"payload"`
	Ordinal int    `json:"ordinal"`
}

const sessionIRHistoryExistingBatchSQL = `SELECT event_id,sequence::text,turn_id,actor,payload
 FROM session_ir_events WHERE session_id=$1 AND event_id IN
 (SELECT value->>'event_id' FROM jsonb_array_elements($2::jsonb))`
const sessionIRHistoryInsertBatchSQL = `WITH supplied AS MATERIALIZED (
 SELECT * FROM jsonb_to_recordset($2::jsonb) AS e(event_id text,payload text,ordinal integer)
), base AS MATERIALIZED (
 SELECT coalesce(max(sequence),0) AS last FROM session_ir_events WHERE session_id=$1
), inserted AS (
 INSERT INTO session_ir_events(session_id,sequence,event_id,turn_id,actor,payload)
 SELECT $1,base.last+row_number() OVER(ORDER BY supplied.ordinal),supplied.event_id,$3,$4,supplied.payload
 FROM supplied CROSS JOIN base ORDER BY supplied.ordinal
 RETURNING event_id,sequence
) SELECT event_id,sequence::text FROM inserted ORDER BY inserted.sequence`

func sessionIRHistoryAppendBatchAuthorized(ctx context.Context, q store.Queryer, f, events []string, allowToolSource, allowInput bool) (uint32, []string, error) {
	ctx, finishTiming := startHistoryTiming(ctx, historyTimingAppend, -1)
	defer finishTiming()
	// Check the complete generated replacement before any store access, exactly as
	// the individual append path does. These events are only message/commit history.
	entries := make([]historyAppendEntry, 0, len(events))
	for index, event := range events {
		entry := historyAppendEntry{f[1] + ":" + strconv.Itoa(index), event, index}
		kind, valid := sessionIRAppendInputValid([]string{f[0], entry.ID, f[1], f[3], event}, false, false, false, false, allowToolSource, allowInput)
		if !valid || kind != "history" {
			return store.StatusInvalid, nil, nil
		}
		entries = append(entries, entry)
	}
	var owner string
	if err := q.QueryRow(ctx, sessionIRLockSQL, f[0]).Scan(&owner); err != nil {
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		return 0, nil, err
	}
	if owner != f[3] {
		return store.StatusInvalid, nil, nil
	}
	// Ordinary primary-history input checks are unchanged; exact replays are
	// checked first, so a later recovery fence never invalidates an existing event.
	for offset := 0; offset < len(entries); {
		end := offset
		size := 0
		for end < len(entries) && end-offset < historyAppendBatchCount {
			encoded, err := json.Marshal(entries[end])
			if err != nil {
				return 0, nil, err
			}
			if end > offset && size+len(encoded) > historyAppendBatchBytes {
				break
			}
			size += len(encoded)
			end++
		}
		status, err := sessionIRHistoryAppendChunk(ctx, q, f, entries[offset:end], allowToolSource, allowInput)
		if err != nil || status != store.StatusOK {
			return status, nil, err
		}
		offset = end
	}
	return store.StatusOK, nil, nil
}
func sessionIRHistoryAppendChunk(ctx context.Context, q store.Queryer, f []string, entries []historyAppendEntry, allowToolSource, allowInput bool) (uint32, error) {
	encoded, err := json.Marshal(entries)
	if err != nil {
		return 0, err
	}
	// A single oversized event retains the old bounded individual append path.
	if len(encoded) > historyAppendBatchBytes {
		for _, e := range entries {
			status, _, err := sessionIRAppendInputAuthorized(ctx, q, []string{f[0], e.ID, f[1], f[3], e.Payload}, false, false, false, false, allowToolSource, allowInput)
			if err != nil || status != store.StatusOK {
				return status, err
			}
		}
		return store.StatusOK, nil
	}
	expected := make(map[string]historyAppendEntry, len(entries))
	for _, e := range entries {
		expected[e.ID] = e
	}
	rows, err := q.Query(ctx, sessionIRHistoryExistingBatchSQL, f[0], string(encoded))
	if err != nil {
		return 0, err
	}
	seen := map[string]bool{}
	for rows.Next() {
		var id, sequence, turn, actor, payload string
		if err = rows.Scan(&id, &sequence, &turn, &actor, &payload); err != nil {
			break
		}
		e, ok := expected[id]
		n, parseErr := strconv.ParseInt(sequence, 10, 64)
		if !ok || seen[id] || parseErr != nil || n < 1 || strconv.FormatInt(n, 10) != sequence {
			err = fmt.Errorf("invalid checkpoint replay row")
			break
		}
		if turn != f[1] || actor != f[3] || payload != e.Payload {
			rows.Close()
			return store.StatusInvalid, nil
		}
		seen[id] = true
	}
	rowErr := rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	if rowErr != nil {
		return 0, rowErr
	}
	missing := make([]historyAppendEntry, 0, len(entries))
	for _, e := range entries {
		if !seen[e.ID] {
			missing = append(missing, e)
		}
	}
	if len(missing) == 0 {
		return store.StatusOK, nil
	}
	if !allowInput {
		allowed, err := ordinaryInputHistoryAllowed(ctx, q, f[0], f[2])
		if err != nil {
			return 0, err
		}
		if !allowed {
			return store.StatusInvalid, nil
		}
	}
	fenced, err := nativeRecoveryFenced(ctx, q, f[0], f[1])
	if err != nil {
		return 0, err
	}
	if fenced {
		return store.StatusInvalid, nil
	}
	encoded, err = json.Marshal(missing)
	if err != nil {
		return 0, err
	}
	rows, err = q.Query(ctx, sessionIRHistoryInsertBatchSQL, f[0], string(encoded), f[1], f[3])
	if err != nil {
		return 0, err
	}
	inserted := map[string]int64{}
	wanted := map[string]bool{}
	for _, e := range missing {
		wanted[e.ID] = true
	}
	var previous int64
	for rows.Next() {
		var id, sequence string
		if err = rows.Scan(&id, &sequence); err != nil {
			break
		}
		n, parseErr := strconv.ParseInt(sequence, 10, 64)
		if len(inserted) >= len(missing) || id != missing[len(inserted)].ID || !wanted[id] || inserted[id] != 0 || parseErr != nil || n < 1 || strconv.FormatInt(n, 10) != sequence || (previous != 0 && n != previous+1) {
			err = fmt.Errorf("invalid checkpoint append result")
			break
		}
		inserted[id] = n
		previous = n
	}
	rowErr = rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	if rowErr != nil {
		return 0, rowErr
	}
	if len(inserted) != len(missing) {
		return store.StatusFailed, fmt.Errorf("incomplete checkpoint append result")
	}
	for _, e := range missing {
		if err = enqueueLearningProjection(ctx, q, []string{f[0], e.ID, f[1], f[3], e.Payload}, inserted[e.ID]); err != nil {
			return store.StatusFailed, err
		}
	}
	return store.StatusOK, nil
}
