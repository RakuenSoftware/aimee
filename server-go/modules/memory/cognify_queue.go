package memory

import (
	"context"
	"encoding/json"
	"errors"

	store "github.com/JBailes/aimee/server-go/db"
)

type cognifyQueueStats struct {
	Status    string `json:"status"`
	Pending   int    `json:"pending"`
	Running   int    `json:"running"`
	Done      int    `json:"done"`
	Failed    int    `json:"failed"`
	Total     int    `json:"total"`
	Processed int    `json:"processed"`
	Retried   int    `json:"retried"`
}

// Queue visibility follows the source memory's RLS, including status counts.
func (s *postgresDataStore) cognifyQueueStatus(ctx context.Context) (cognifyQueueStats, error) {
	stats := cognifyQueueStats{Status: "ok"}
	err := s.db.QueryRow(ctx, `SELECT count(*) FILTER(WHERE status='pending'),count(*) FILTER(WHERE status='running'),
 count(*) FILTER(WHERE status='done'),count(*) FILTER(WHERE status='failed'),count(*)
 FROM kb_async_jobs j JOIN memories m ON m.id=j.document_id WHERE j.kind='memory_cognify'`).
		Scan(&stats.Pending, &stats.Running, &stats.Done, &stats.Failed, &stats.Total)
	return stats, err
}

const cognifyJobReady = `j.kind='memory_cognify' AND j.status IN ('pending','failed') AND j.attempts<3
 AND (j.next_attempt_at='' OR j.next_attempt_at::timestamptz<=clock_timestamp())`

func (s *postgresDataStore) cognifyNext(ctx context.Context, command string) (worked, retried bool, err error) {
	if _, ok := s.db.(store.Tx); !ok {
		return false, false, errors.New("memory: cognification requires a transaction")
	}
	var id int64
	err = s.db.QueryRow(ctx, `SELECT m.id FROM memories m JOIN kb_async_jobs j ON j.document_id=m.id
 WHERE `+cognifyJobReady+` ORDER BY j.id LIMIT 1 FOR UPDATE OF m SKIP LOCKED`).Scan(&id)
	if store.IsNoRows(err) {
		return false, false, nil
	}
	if err != nil {
		return false, false, err
	}
	var job, generation int64
	var attempts int
	err = s.db.QueryRow(ctx, `SELECT j.id,j.generation,j.attempts FROM kb_async_jobs j WHERE j.document_id=$1 AND `+cognifyJobReady+` FOR UPDATE SKIP LOCKED`, id).Scan(&job, &generation, &attempts)
	if store.IsNoRows(err) {
		return false, false, nil
	}
	if err != nil {
		return false, false, err
	}
	rewindAudit := s.auditSavepoint()
	if _, err = s.db.Exec(ctx, `SAVEPOINT memory_cognify_attempt`); err != nil {
		return false, false, err
	}
	// Execute synchronously even when the enqueue option is enabled. The parent
	// and generation locks survive the savepoint and protect the full model call.
	_, workErr := s.cognify(ctx, id, command, false)
	if workErr != nil {
		rewindAudit()
		if _, err = s.db.Exec(ctx, `ROLLBACK TO SAVEPOINT memory_cognify_attempt`); err != nil {
			return false, false, err
		}
	}
	if _, err = s.db.Exec(ctx, `RELEASE SAVEPOINT memory_cognify_attempt`); err != nil {
		return false, false, err
	}
	if workErr != nil {
		// Keep model output, commands and database error details out of durable jobs.
		_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='failed',attempts=attempts+1,last_error='cognification attempt failed',
 next_attempt_at=(clock_timestamp()+make_interval(secs=>5*(attempts+1)))::text,updated_at=pg_now_text()
 WHERE id=$1 AND generation=$2`, job, generation)
		return true, attempts+1 < 3, err
	}
	_, err = s.db.Exec(ctx, `UPDATE kb_async_jobs SET status='done',attempts=0,last_error='',next_attempt_at='',updated_at=pg_now_text()
 WHERE id=$1 AND generation=$2`, job, generation)
	return true, false, err
}

func (s *postgresDataStore) cognifyData(ctx context.Context, request DataRequest) (json.RawMessage, error) {
	if request.Operation == "cognify-status" {
		stats, err := s.cognifyQueueStatus(ctx)
		if err != nil {
			return nil, err
		}
		return json.Marshal(stats)
	}
	command, async, err := s.cognifySettings()
	if errors.Is(err, errCognifyDisabled) {
		if request.Operation == "cognify-drain" {
			return json.Marshal(cognifyQueueStats{Status: "disabled"})
		}
		return json.Marshal(map[string]any{"status": "error", "kind": "disabled", "message": errCognifyDisabled.Error()})
	}
	if err != nil {
		return nil, err
	}
	if request.Operation == "cognify" {
		out, err := s.cognify(ctx, request.ID, command, async)
		if errors.Is(err, ErrMemoryNotFound) {
			return json.Marshal(map[string]any{"status": "error", "kind": "not_found", "message": "memory not found"})
		}
		if err != nil {
			return nil, err
		}
		return json.Marshal(out)
	}
	processed, retries := 0, 0
	for i := 0; i < request.Limit; i++ {
		worked, retried, err := s.cognifyNext(ctx, command)
		if err != nil {
			return nil, err
		}
		if !worked {
			break
		}
		processed++
		if retried {
			retries++
		}
	}
	stats, err := s.cognifyQueueStatus(ctx)
	if err != nil {
		return nil, err
	}
	stats.Processed, stats.Retried = processed, retries
	return json.Marshal(stats)
}
