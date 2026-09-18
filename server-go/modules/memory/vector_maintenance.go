package memory

import (
	"context"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

const vectorRebuildLock int64 = 0x41494d4545564543

// DDL belongs to the schema owner. A runtime rebuild clears derived rows in a
// transaction and retains the deployed vector type, dimension and ANN indexes.
func (s *postgresDataStore) VectorCollectionExists(ctx context.Context) (bool, error) {
	var exists bool
	err := s.db.QueryRow(ctx, `SELECT EXISTS (
 SELECT 1 FROM pg_index i JOIN pg_class idx ON idx.oid=i.indexrelid
 JOIN pg_am am ON am.oid=idx.relam
 WHERE i.indrelid=to_regclass('memory_embeddings') AND i.indisvalid
 AND am.amname IN ('hnsw','diskann'))`).Scan(&exists)
	return exists, err
}

func (s *postgresDataStore) vectorDimension(ctx context.Context) (int, error) {
	var dimension int
	err := s.db.QueryRow(ctx, `SELECT atttypmod FROM pg_attribute
WHERE attrelid=to_regclass('memory_embeddings') AND attname='embedding' AND NOT attisdropped`).Scan(&dimension)
	if err != nil {
		return 0, err
	}
	if dimension < 1 || dimension > 4000 {
		return 0, errors.New("memory: unsupported deployed embedding dimension")
	}
	return dimension, nil
}

func (s *postgresDataStore) vectorTransaction(ctx context.Context, run func(*postgresDataStore) error) error {
	if err := s.requireKBDomain(); err != nil {
		return err
	}
	if db, ok := s.db.(store.DB); ok {
		tx, err := db.Begin(ctx)
		if err != nil {
			return err
		}
		defer tx.Rollback(context.Background())
		bound := *s
		bound.db = tx
		if err := bound.vectorTransaction(ctx, run); err != nil {
			return err
		}
		return tx.Commit(ctx)
	}
	if _, ok := s.db.(store.Tx); !ok {
		return errors.New("memory: vector rebuild requires a transaction")
	}
	var acquired bool
	if err := s.db.QueryRow(ctx, `SELECT pg_try_advisory_xact_lock($1)`, vectorRebuildLock).Scan(&acquired); err != nil {
		return err
	}
	if !acquired {
		return errors.New("memory: another vector rebuild is running")
	}
	return run(s)
}

func (s *postgresDataStore) clearVectorCollection(ctx context.Context, dim int) error {
	actual, err := s.vectorDimension(ctx)
	if err != nil {
		return err
	}
	if actual != dim {
		return fmt.Errorf("memory: requested dimension %d differs from deployed dimension %d", dim, actual)
	}
	exists, err := s.VectorCollectionExists(ctx)
	if err != nil {
		return err
	}
	if !exists {
		return errors.New("memory: vector index unavailable; schema-owner maintenance is required")
	}
	var allScopes bool
	if err = s.db.QueryRow(ctx, `SELECT COALESCE(current_setting('aimee.memory_scope_all',true),'')='1'`).Scan(&allScopes); err != nil {
		return err
	}
	if !allScopes {
		return errors.New("memory: vector collection reset requires all-scope maintenance")
	}
	_, err = s.db.Exec(ctx, `DELETE FROM memory_embeddings WHERE record_type IN ('memory','unit')`)
	return err
}

func (s *postgresDataStore) RecreateVectorCollection(ctx context.Context, dim int) error {
	if dim < 1 || dim > 4000 {
		return errors.New("memory: invalid vector dimension")
	}
	return s.vectorTransaction(ctx, func(bound *postgresDataStore) error { return bound.clearVectorCollection(ctx, dim) })
}

func (s *postgresDataStore) RebuildVectorIndex(ctx context.Context, version string) (int, int, error) {
	if version == "" {
		return 0, 0, errors.New("memory: embedder version is required")
	}
	queued := 0
	err := s.vectorTransaction(ctx, func(bound *postgresDataStore) error {
		active, _, _, err := bound.activeEmbeddingVersion(ctx)
		if err != nil {
			return err
		}
		if active != "" && active != version {
			return errors.New("memory: rebuild version is not active; use a validated cutover or rollback")
		}
		dim, err := bound.vectorDimension(ctx)
		if err != nil {
			return err
		}
		if err := bound.clearVectorCollection(ctx, dim); err != nil {
			return err
		}
		if err := bound.db.QueryRow(ctx, `WITH q AS (
 INSERT INTO vector_index_ops(point_id,collection,memory_id,status,attempts,last_error,updated_at)
 SELECT point_id,'memory',memory_id,'pending',0,'',pg_now_text() FROM (
 SELECT id AS point_id,id AS memory_id FROM memories WHERE lifecycle_state='active'
 UNION ALL SELECT $1+u.id,u.memory_id FROM memory_units u JOIN memories m ON m.id=u.memory_id
 WHERE m.lifecycle_state='active') points
 ON CONFLICT(point_id) DO UPDATE SET collection=EXCLUDED.collection,memory_id=EXCLUDED.memory_id,
 status='pending',attempts=0,last_error='',updated_at=pg_now_text()
 RETURNING 1) SELECT count(*) FROM q`, unitPointOffset).Scan(&queued); err != nil {
			return err
		}
		_, err = bound.db.Exec(ctx, `INSERT INTO kb_meta(key,value) VALUES('vector_schema_version',$2),('memory_vector_rebuild_version',$1)
 ON CONFLICT(key) DO UPDATE SET value=EXCLUDED.value`, version, vectorSchemaVersion)
		return err
	})
	if err != nil {
		return 0, 0, err
	}
	return queued, 0, nil
}
