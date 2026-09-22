package memory

import (
	"context"
	"errors"
	"strings"

	store "github.com/JBailes/aimee/server-go/db"
)

// RebuildDerivedIndexes runs under the caller's RLS context. The outer
// transaction makes replacement all-or-nothing, including vector queue writes.
func (s *postgresDataStore) RebuildDerivedIndexes(ctx context.Context, limit int) (int, error) {
	if s.placement != PlacementKB {
		return 0, errors.New("memory: derived indexes belong to KB placement")
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return 0, err
		}
		tx = s.auditTransaction(tx)
		defer tx.Rollback(context.Background())
		bound := *s
		bound.db = tx
		count, err := bound.RebuildDerivedIndexes(ctx, limit)
		if err != nil {
			return 0, err
		}
		return count, tx.Commit(ctx)
	}
	if _, ok := s.db.(store.Tx); !ok {
		return 0, errors.New("memory: derived indexes require a transaction")
	}
	settings, err := s.derivedSettings()
	if err != nil {
		return 0, err
	}
	if limit <= 0 || limit > 100000 {
		limit = 100000
	}
	rows, err := s.db.Query(ctx, `SELECT id FROM memories WHERE lifecycle_state='active' ORDER BY id LIMIT $1 FOR UPDATE`, limit)
	if err != nil {
		return 0, err
	}
	ids := []int64{}
	for rows.Next() {
		var id int64
		if err = rows.Scan(&id); err != nil {
			rows.Close()
			return 0, err
		}
		ids = append(ids, id)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return 0, err
	}
	for _, id := range ids {
		if err = s.refreshDerivedRecord(ctx, id, settings); err != nil {
			return 0, err
		}
	}
	return len(ids), nil
}

// The caller holds the parent lock and transaction for the complete replacement.
// Background jobs and explicit reindex use this same pipeline.
func (s *postgresDataStore) refreshDerivedRecord(ctx context.Context, id int64, settings derivedSettings) error {
	var err error
	var key, content, created string
	if err = s.db.QueryRow(ctx, `SELECT key,content,created_at FROM memories WHERE id=$1`, id).Scan(&key, &content, &created); err != nil {
		return err
	}
	if err = s.replaceDerivedText(ctx, id, deriveText(key, content, created)); err != nil {
		return err
	}

	if settings.Negation {
		if _, err = s.db.Exec(ctx, `UPDATE memories SET negation_tokens=$2 WHERE id=$1`, id, strings.Join(negationTokens(textBound(key+" "+content, 3071)), " ")); err != nil {
			return err
		}
	}
	if err = s.refreshCoreference(ctx, id, content, settings); err != nil {
		return err
	}
	if err = s.replaceDerivedRelations(ctx, id); err != nil {
		return err
	}
	if err = s.replaceDerivedUnits(ctx, id); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_scopes(memory_id,scope_type,scope_value)
 SELECT id,scope_type,scope_value FROM memories WHERE id=$1 ON CONFLICT DO NOTHING`, id); err != nil {
		return err
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,updated_at)
 SELECT id,'memory',id,'pending',0,'',pg_now_text() FROM memories WHERE id=$1
 ON CONFLICT(point_id) DO UPDATE SET status='pending',attempts=0,last_error='',updated_at=pg_now_text()`, id); err != nil {
		return err
	}
	return nil
}
