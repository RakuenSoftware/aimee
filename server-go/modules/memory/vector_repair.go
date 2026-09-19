package memory

import (
	"context"
	"errors"
	"os"
	"strconv"

	store "github.com/JBailes/aimee/server-go/db"
)

func vectorRetryLimit() int {
	if n, err := strconv.Atoi(os.Getenv("AIMEE_VECTOR_MAX_RETRY")); err == nil && n >= 1 && n <= 1024 {
		return n
	}
	return 8
}

func (s *postgresDataStore) embeddingCommand(requested string) (string, error) {
	if requested == "" && s.placement == PlacementKB && s.db != nil {
		version, command, _, err := s.activeEmbeddingVersion(context.Background())
		if err != nil {
			return "", err
		}
		if version != "" {
			return command, nil
		}
	}
	if requested != "" {
		return requested, nil
	}
	values := map[string]any{}
	if s.settings != nil {
		var err error
		values, err = s.settings()
		if err != nil {
			return "", err
		}
	}
	text := func(key string) string { value, _ := values[key].(string); return value }
	if url := text("embedder_url"); url != "" {
		return url, nil
	}
	if text("embedder_model") != "" {
		return "https://aimee-embedder:8762", nil
	}
	if url := os.Getenv("EMBEDDER_URL"); url != "" {
		return url, nil
	}
	return text("embedder_command"), nil
}

func (s *postgresDataStore) prepareVectorRepair(ctx context.Context, request DataRequest) (DataResponse, error) {
	if err := s.requireKBDomain(); err != nil {
		return DataResponse{}, err
	}
	if request.ResetStuck {
		// Preserve the operator's existing recovery command for memory and
		// code work queues. Retrying never changes the canonical records.
		count := 0
		for _, table := range []string{"vector_index_ops", "code_index_ops"} {
			tag, err := s.db.Exec(ctx, `UPDATE `+table+` SET attempts=0 WHERE status='failed' AND attempts >= $1`, vectorRetryLimit())
			if err != nil {
				return DataResponse{}, err
			}
			count += int(tag.RowsAffected())
		}
		return DataResponse{Count: &count}, nil
	}
	dimension, err := s.vectorDimension(ctx)
	if err != nil {
		return DataResponse{}, err
	}
	exists, err := s.VectorCollectionExists(ctx)
	if err != nil {
		return DataResponse{}, err
	}
	if !exists {
		return DataResponse{}, errors.New("memory: vector index unavailable; schema-owner maintenance is required")
	}
	response := DataResponse{Dimension: dimension, IDs: []int64{}}
	if request.ID > 0 {
		response.IDs = []int64{request.ID}
		return response, nil
	}
	if request.FailedOnly {
		response.IDs, err = s.FailedEmbeddingIDs(ctx, request.Limit)
		return response, err
	}
	rows, err := s.db.Query(ctx, `SELECT point FROM (SELECT id AS point,updated_at FROM memories WHERE lifecycle_state='active'
 UNION ALL SELECT $2+u.id,m.updated_at FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE m.lifecycle_state='active') points ORDER BY updated_at DESC,point DESC LIMIT $1`, request.Limit, unitPointOffset)
	if err != nil {
		return DataResponse{}, err
	}
	defer rows.Close()
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return DataResponse{}, err
		}
		response.IDs = append(response.IDs, id)
	}
	return response, rows.Err()
}

// Keep a failed vector write from aborting its enclosing request transaction:
// the caller can then persist a retry diagnostic. Both vector and work-queue
// updates commit together; a failure of either rolls the pair back.
func (s *postgresDataStore) UpsertEmbedding(ctx context.Context, record Record, vector []float32) error {
	return s.withEmbeddingWrite(ctx, func(bound *postgresDataStore) error { return bound.upsertEmbedding(ctx, record, vector) })
}

func (s *postgresDataStore) withEmbeddingWrite(ctx context.Context, write func(*postgresDataStore) error) error {
	if s.placement != PlacementKB {
		return errors.New("personal vectors require the owner-selected serving identity")
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return err
		}
		tx = s.auditTransaction(tx)
		defer tx.Rollback(context.Background())
		bound := *s
		bound.db = tx
		if err := bound.withEmbeddingWrite(ctx, write); err != nil {
			return err
		}
		return tx.Commit(ctx)
	}
	if _, ok := s.db.(store.Tx); !ok {
		return errors.New("memory: embedding persistence requires a transaction")
	}
	rewindAudit := s.auditSavepoint()
	if _, err := s.db.Exec(ctx, `SAVEPOINT memory_embedding_write`); err != nil {
		return err
	}
	err := write(s)
	if err != nil {
		// Rollback must still run if the embedding request was cancelled.
		rewindAudit()
		_, _ = s.db.Exec(context.Background(), `ROLLBACK TO SAVEPOINT memory_embedding_write`)
	}
	_, releaseErr := s.db.Exec(context.Background(), `RELEASE SAVEPOINT memory_embedding_write`)
	if err != nil {
		return err
	}
	return releaseErr
}

func (s *postgresDataStore) prepareVectorEmbed(ctx context.Context, request DataRequest) (DataResponse, error) {
	prepared, err := s.prepareVectorRepair(ctx, request)
	if err != nil || request.ID > 0 {
		return prepared, err
	}
	var active string
	if err = s.db.QueryRow(ctx, `SELECT COALESCE((SELECT version FROM memory_active_embedder WHERE id=1),'')`).Scan(&active); err != nil {
		return DataResponse{}, err
	}
	if active != "" && request.Version != "" && request.Version != active {
		return DataResponse{}, errors.New("memory: requested version is not active; use reembed_start")
	}
	rows, err := s.db.Query(ctx, `SELECT point FROM (
 SELECT m.id AS point FROM memories m WHERE lifecycle_state='active'
 UNION ALL SELECT $1+u.id FROM memory_units u JOIN memories m ON m.id=u.memory_id WHERE m.lifecycle_state='active') candidates
 LEFT JOIN memory_embeddings e ON e.point_id=point LEFT JOIN vector_index_ops o ON o.point_id=point
 WHERE e.point_id IS NULL OR o.status IN ('pending','failed') ORDER BY point LIMIT $2`, unitPointOffset, request.Limit)
	if err != nil {
		return DataResponse{}, err
	}
	defer rows.Close()
	prepared.IDs = []int64{}
	for rows.Next() {
		var point int64
		if err = rows.Scan(&point); err != nil {
			return DataResponse{}, err
		}
		prepared.IDs = append(prepared.IDs, point)
	}
	return prepared, rows.Err()
}
