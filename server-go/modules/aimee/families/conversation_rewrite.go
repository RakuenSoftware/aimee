package families

import (
	"context"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// Payload rewrite state: one row per session, tracking how often its prompt
// payload has been rewritten and how much rewriting has been deferred.

const rewriteColumns = `session_id, payload_epoch, compaction_epoch, last_prefix_hash,
                        last_payload_tokens, last_rewrite_at, deferred_rewrite_count,
                        consecutive_deferred_count, bytes_saved_pending, rewrite_reason,
                        to_char(updated_at AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS')`

const (
	rewriteGetSQL = `SELECT ` + rewriteColumns + `
	                   FROM payload_rewrite_state WHERE session_id = $1`

	// An upsert rather than the C's INSERT OR REPLACE. The two agree here only
	// because every column is listed; REPLACE deletes the row and inserts a new
	// one, so it would silently drop any column added to this table later
	// without being added to this statement. ON CONFLICT touches only what it
	// names.
	rewriteSetSQL = `INSERT INTO payload_rewrite_state
	                     (session_id, payload_epoch, compaction_epoch, last_prefix_hash,
	                      last_payload_tokens, last_rewrite_at, deferred_rewrite_count,
	                      consecutive_deferred_count, bytes_saved_pending, rewrite_reason,
	                      updated_at)
	                 VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9, $10, now())
	                 ON CONFLICT (session_id) DO UPDATE SET
	                     payload_epoch              = EXCLUDED.payload_epoch,
	                     compaction_epoch           = EXCLUDED.compaction_epoch,
	                     last_prefix_hash           = EXCLUDED.last_prefix_hash,
	                     last_payload_tokens        = EXCLUDED.last_payload_tokens,
	                     last_rewrite_at            = EXCLUDED.last_rewrite_at,
	                     deferred_rewrite_count     = EXCLUDED.deferred_rewrite_count,
	                     consecutive_deferred_count = EXCLUDED.consecutive_deferred_count,
	                     bytes_saved_pending        = EXCLUDED.bytes_saved_pending,
	                     rewrite_reason             = EXCLUDED.rewrite_reason,
	                     updated_at                 = now()`

	// A deferred rewrite accumulates: the counts rise and the pending bytes add
	// up, because nothing was actually rewritten and the saving is still owed.
	rewriteRecordDeferredSQL = `INSERT INTO payload_rewrite_state
	                                (session_id, deferred_rewrite_count,
	                                 consecutive_deferred_count, bytes_saved_pending,
	                                 last_payload_tokens, last_rewrite_at,
	                                 rewrite_reason, last_prefix_hash, updated_at)
	                            VALUES ($1, 1, 1, $2, $3,
	                                    to_char(now() AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
	                                    $4, $5, now())
	                            ON CONFLICT (session_id) DO UPDATE SET
	                                deferred_rewrite_count =
	                                    payload_rewrite_state.deferred_rewrite_count + 1,
	                                consecutive_deferred_count =
	                                    payload_rewrite_state.consecutive_deferred_count + 1,
	                                bytes_saved_pending =
	                                    payload_rewrite_state.bytes_saved_pending
	                                    + EXCLUDED.bytes_saved_pending,
	                                last_payload_tokens = EXCLUDED.last_payload_tokens,
	                                last_rewrite_at     = EXCLUDED.last_rewrite_at,
	                                rewrite_reason      = EXCLUDED.rewrite_reason,
	                                last_prefix_hash    = EXCLUDED.last_prefix_hash,
	                                updated_at          = now()`

	// A rewrite that actually happened advances the epoch and CLEARS the
	// deferred run: the pending bytes were just realised, so they are no longer
	// owed and the consecutive count starts again from nothing.
	rewriteRecordAppliedSQL = `INSERT INTO payload_rewrite_state
	                               (session_id, payload_epoch, consecutive_deferred_count,
	                                bytes_saved_pending, last_payload_tokens, last_rewrite_at,
	                                rewrite_reason, last_prefix_hash, updated_at)
	                           VALUES ($1, 1, 0, 0, $2,
	                                   to_char(now() AT TIME ZONE 'utc', 'YYYY-MM-DD HH24:MI:SS'),
	                                   $3, $4, now())
	                           ON CONFLICT (session_id) DO UPDATE SET
	                               payload_epoch =
	                                   payload_rewrite_state.payload_epoch + 1,
	                               consecutive_deferred_count = 0,
	                               bytes_saved_pending        = 0,
	                               last_payload_tokens        = EXCLUDED.last_payload_tokens,
	                               last_rewrite_at            = EXCLUDED.last_rewrite_at,
	                               rewrite_reason             = EXCLUDED.rewrite_reason,
	                               last_prefix_hash           = EXCLUDED.last_prefix_hash,
	                               updated_at                 = now()`
)

// rewriteRow reads the state as its eleven wire cells.
func rewriteRow(scan func(...any) error) ([]string, error) {
	var (
		sessionID, lastPrefixHash, lastRewriteAt, reason, updatedAt string
		payloadEpoch, compactionEpoch, lastPayloadTokens            int64
		deferredCount, consecutiveDeferred, bytesSavedPending       int64
	)
	if err := scan(&sessionID, &payloadEpoch, &compactionEpoch, &lastPrefixHash,
		&lastPayloadTokens, &lastRewriteAt, &deferredCount,
		&consecutiveDeferred, &bytesSavedPending, &reason, &updatedAt); err != nil {
		return nil, err
	}
	return []string{
		sessionID,
		store.I64toa(payloadEpoch),
		store.I64toa(compactionEpoch),
		lastPrefixHash,
		store.I64toa(lastPayloadTokens),
		lastRewriteAt,
		store.I64toa(deferredCount),
		store.I64toa(consecutiveDeferred),
		store.I64toa(bytesSavedPending),
		reason,
		updatedAt,
	}, nil
}

func rewriteStateGet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	reply, err := rewriteRow(func(dest ...any) error {
		return q.QueryRow(ctx, rewriteGetSQL, f[0]).Scan(dest...)
	})
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, reply, nil
}

func rewriteStateSet(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	// f[10] is the caller's updated_at, which the store sets itself; it is
	// accepted on the wire and ignored, exactly as the C did.
	nums := make([]int64, 0, 6)
	for _, at := range []int{1, 2, 4, 6, 7, 8} {
		n, ok := store.Atoi64(f[at])
		if !ok {
			return store.StatusInvalid, nil, nil
		}
		nums = append(nums, n)
	}
	_, err := q.Exec(ctx, rewriteSetSQL,
		f[0],    // session_id
		nums[0], // payload_epoch
		nums[1], // compaction_epoch
		f[3],    // last_prefix_hash
		nums[2], // last_payload_tokens
		f[5],    // last_rewrite_at
		nums[3], // deferred_rewrite_count
		nums[4], // consecutive_deferred_count
		nums[5], // bytes_saved_pending
		f[9])    // rewrite_reason
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}

// rewriteRecord notes one rewrite attempt, deferred or applied.
func rewriteRecord(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	deferred, ok := store.Atoi64(f[1])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	bytesSaved, ok := store.Atoi64(f[2])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	tokens, ok := store.Atoi64(f[3])
	if !ok {
		return store.StatusInvalid, nil, nil
	}
	reason, prefixHash := f[4], f[5]

	var err error
	if deferred != 0 {
		_, err = q.Exec(ctx, rewriteRecordDeferredSQL, f[0], bytesSaved, tokens, reason, prefixHash)
	} else {
		// The applied form does not take bytesSaved: the saving is realised, so
		// there is nothing left pending to carry.
		_, err = q.Exec(ctx, rewriteRecordAppliedSQL, f[0], tokens, reason, prefixHash)
	}
	if err != nil {
		return store.StatusFailed, nil, err
	}
	return store.StatusOK, nil, nil
}
