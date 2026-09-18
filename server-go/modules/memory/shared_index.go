package memory

import (
	"context"
	"errors"
	"fmt"
	"log"
	"time"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// StartSharedIndex runs inside the same Go owner as the personal index. Only KB
// placement consumes shared jobs, using the restricted store role and governed
// egress. Cancellation follows the module process lifecycle.
func StartSharedIndex(ctx context.Context, data DataStore, executor egress.Executor) {
	s, ok := data.(*postgresDataStore)
	if !ok || ctx == nil || s.placement != PlacementKB {
		return
	}
	if _, ok := s.db.(store.DB); !ok {
		return
	}
	go func() {
		ticker := time.NewTicker(5 * time.Second)
		defer ticker.Stop()
		for ctx.Err() == nil {
			attempt, cancel := context.WithTimeout(ctx, 60*time.Second)
			err := s.sharedIndexBatch(attempt, executor, 16)
			cancel()
			if err != nil && ctx.Err() == nil {
				log.Printf("shared memory indexing pending: %v", err)
			}
			select {
			case <-ctx.Done():
				return
			case <-ticker.C:
			}
		}
	}()
}

// Internal queue processing deliberately spans scopes. This is never exposed as
// a request flag or a public command; each attempt gets a fresh transaction and
// the privilege ends with it. Derived edges still enforce their own scope rules.
func sharedIndexContext(ctx context.Context, tx store.Tx) error {
	_, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true),
 set_config('aimee.memory_scope_type','',true),set_config('aimee.memory_scope_value','',true),
 set_config('aimee.memory_workspace','',true),set_config('aimee.memory_project','',true),
 set_config('aimee.principal','system:memory-index',true),set_config('aimee.authority','model',true),
 set_config('aimee.transport_identity','internal',true),set_config('aimee.correlation_id','',true)`)
	return err
}

func (s *postgresDataStore) sharedIndexBatch(ctx context.Context, executor egress.Executor, limit int) error {
	if s.placement != PlacementKB {
		return errors.New("memory: shared indexing requires KB placement")
	}
	if limit <= 0 || limit > 64 {
		limit = 16
	}
	db, ok := s.db.(store.DB)
	if !ok {
		return errors.New("memory: shared indexing requires transactions")
	}
	run := func(vector bool, command string) (worked bool, err error) {
		tx, err := db.Begin(ctx)
		if err != nil {
			return false, err
		}
		defer tx.Rollback(context.Background())
		if err = sharedIndexContext(ctx, tx); err != nil {
			return false, err
		}
		bound := *s
		bound.db = tx
		if command == "memory-cognify" {
			configured, _, configErr := bound.cognifySettings()
			if errors.Is(configErr, errCognifyDisabled) {
				return false, nil
			}
			if configErr != nil {
				return false, configErr
			}
			worked, _, err = bound.cognifyNext(ctx, configured)
		} else if vector {
			worked, err = bound.indexSharedVector(ctx, executor, command)
		} else {
			worked, err = bound.indexSharedRecord(ctx)
		}
		if err != nil {
			return worked, err
		}
		return worked, tx.Commit(ctx)
	}
	for i := 0; i < limit; i++ {
		worked, err := run(false, "")
		if err != nil {
			return err
		}
		if !worked {
			break
		}
	}
	for i := 0; i < limit; i++ {
		worked, err := run(false, "memory-cognify")
		if err != nil {
			return err
		}
		if !worked {
			break
		}
	}
	command, err := s.embeddingCommand("")
	if err != nil || command == "" {
		return err
	}
	for i := 0; i < limit; i++ {
		worked, err := run(true, command)
		if err != nil {
			return err
		}
		if !worked {
			break
		}
	}
	return nil
}

const sharedJobReady = `j.kind='memory_index' AND j.status IN ('pending','failed') AND j.attempts<8
 AND (j.next_attempt_at='' OR j.next_attempt_at::timestamptz<=clock_timestamp())`

func (s *postgresDataStore) indexSharedRecord(ctx context.Context) (bool, error) {
	// Physical deletion can remove the parent before its job is claimed. Queue
	// ownership retains the point IDs needed to clean up orphan unit vectors.
	if _, err := s.db.Exec(ctx, `WITH orphan AS MATERIALIZED (
 SELECT document_id FROM kb_async_jobs j WHERE kind='memory_index' AND status<>'done'
 AND NOT EXISTS(SELECT 1 FROM memories m WHERE m.id=j.document_id)
), vectors AS (DELETE FROM memory_embeddings WHERE point_id IN (SELECT point_id FROM vector_index_ops
 WHERE memory_id IN(SELECT document_id FROM orphan)) OR point_id IN(SELECT document_id FROM orphan) RETURNING point_id),
 jobs AS (DELETE FROM vector_index_ops WHERE memory_id IN(SELECT document_id FROM orphan) RETURNING point_id)
 UPDATE kb_async_jobs SET status='done',last_error='',next_attempt_at='' WHERE kind='memory_index'
 AND document_id IN(SELECT document_id FROM orphan)`); err != nil {
		return false, err
	}
	// Writers acquire parent then enqueue. Follow the same order; SKIP LOCKED
	// permits multiple Go owners without duplicate execution or stalled batches.
	var id int64
	err := s.db.QueryRow(ctx, `SELECT m.id FROM memories m JOIN kb_async_jobs j ON j.document_id=m.id
 WHERE `+sharedJobReady+` ORDER BY j.id LIMIT 1 FOR UPDATE OF m SKIP LOCKED`).Scan(&id)
	if store.IsNoRows(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var job, generation int64
	err = s.db.QueryRow(ctx, `SELECT j.id,j.generation FROM kb_async_jobs j WHERE j.document_id=$1 AND `+sharedJobReady+`
 FOR UPDATE SKIP LOCKED`, id).Scan(&job, &generation)
	if store.IsNoRows(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	var lifecycle string
	if err = s.db.QueryRow(ctx, `SELECT lifecycle_state FROM memories WHERE id=$1`, id).Scan(&lifecycle); err != nil {
		return false, err
	}
	// Keep both locks outside the savepoint. A failed derivation rolls back every
	// index mutation while its retry status commits. Process death rolls back the
	// whole attempt, leaving the original pending work claimable immediately.
	if _, err = s.db.Exec(ctx, `SAVEPOINT memory_shared_index`); err != nil {
		return false, err
	}
	if lifecycle == "active" {
		var settings derivedSettings
		settings, err = s.derivedSettings()
		if err == nil {
			err = s.refreshDerivedRecord(ctx, id, settings)
		}
	} else {
		_, err = s.db.Exec(ctx, `WITH removed AS (DELETE FROM memory_embeddings WHERE point_id=$1 OR
   point_id IN (SELECT $2+id FROM memory_units WHERE memory_id=$1) RETURNING point_id)
   DELETE FROM vector_index_ops WHERE memory_id=$1`, id, unitPointOffset)
	}
	workErr := err
	if workErr != nil {
		if _, err = s.db.Exec(context.Background(), `ROLLBACK TO SAVEPOINT memory_shared_index`); err != nil {
			return false, err
		}
	}
	if _, err = s.db.Exec(context.Background(), `RELEASE SAVEPOINT memory_shared_index`); err != nil {
		return false, err
	}
	if workErr != nil {
		_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='failed',attempts=attempts+1,last_error=$3,
   next_attempt_at=(clock_timestamp()+make_interval(secs=>LEAST(300,5*power(2,attempts))::int))::text,
   updated_at=clock_timestamp()::text WHERE id=$1 AND generation=$2`, job, generation, textBound(workErr.Error(), 1024))
	} else {
		_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='done',attempts=0,last_error='',next_attempt_at='',
   updated_at=clock_timestamp()::text WHERE id=$1 AND generation=$2`, job, generation)
	}
	return true, err
}

func (s *postgresDataStore) indexSharedVector(ctx context.Context, executor egress.Executor, command string) (bool, error) {
	var parent, point, attempts int64
	ready := `v.collection='memory' AND v.status IN ('pending','failed') AND v.attempts<$1
 AND (v.status='pending' OR NULLIF(v.updated_at,'')::timestamptz<=clock_timestamp()-interval '30 seconds')
 AND NOT EXISTS(SELECT 1 FROM kb_async_jobs j WHERE j.kind='memory_index' AND j.document_id=m.id AND j.status<>'done')`
	err := s.db.QueryRow(ctx, `SELECT m.id,v.point_id FROM vector_index_ops v JOIN memories m ON m.id=v.memory_id
 WHERE m.lifecycle_state='active' AND `+ready+` ORDER BY v.updated_at,v.point_id LIMIT 1 FOR UPDATE OF m SKIP LOCKED`, vectorRetryLimit()).Scan(&parent, &point)
	if store.IsNoRows(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	// Recheck after acquiring the parent lock: another owner may have completed
	// this point while we waited for the lock's snapshot to advance.
	err = s.db.QueryRow(ctx, `SELECT v.point_id,v.attempts FROM vector_index_ops v JOIN memories m ON m.id=v.memory_id
 WHERE v.point_id=$2 AND m.lifecycle_state='active' AND `+ready+` FOR UPDATE OF v SKIP LOCKED`, vectorRetryLimit(), point).Scan(&point, &attempts)
	if store.IsNoRows(err) {
		return false, nil
	}
	if err != nil {
		return false, err
	}
	dimension, err := s.vectorDimension(ctx)
	if err != nil {
		return false, err
	}
	result := EmbedRecord(ctx, 0, executor, s, point, command, dimension)
	if !result.Embedded {
		// EmbedRecord normally persists failure itself. Ensure even early validation
		// errors are retryable without incrementing a failed attempt twice.
		_, err = s.db.Exec(ctx, `UPDATE vector_index_ops SET status='failed',attempts=attempts+1,last_error=$2,
   updated_at=pg_now_text() WHERE point_id=$1 AND attempts=$3`, point, textBound(fmt.Sprint(result.Error), 1024), attempts)
	}
	return true, err
}
