package memory

import (
	"context"
	"errors"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Index admission deliberately omits valid/belief time and utility horizons.
// Retained canonical assertions stay searchable in advertised temporal modes;
// processing prohibitions and all memory-parent authorization still apply.
func assertionIndexInputsSQL() string {
	return `SELECT e.id,e.version,e.source||' '||e.relation||' '||e.target||' ['||e.assertion_kind||']' AS input_text,
 encode(sha256(convert_to(jsonb_build_array(e.id,e.version,e.source,e.relation,e.target,e.assertion_kind,
 COALESCE((SELECT jsonb_agg(jsonb_build_array(m.id,m.record_revision) ORDER BY m.id) FROM fact_evidence f
 JOIN memories m ON f.source_id='memory:'||m.id::text WHERE f.assertion_id=e.id AND f.source_kind='memory' AND f.invalidated_at=''),'[]'::jsonb))::text,'UTF8')),'hex') AS input_hash
 FROM entity_edges e WHERE e.edge_class='semantic' AND e.suppressed=0 AND e.lifecycle_state IN ('persistent','promoted','superseded')
 AND ` + memoryEvidenceSQL("e", "TRUE", true, historicalMemoryInspectionSQL("m."))
}
func (s *postgresDataStore) assertionReembedCounts(ctx context.Context, version string) (total, done int64, err error) {
	err = s.db.QueryRow(ctx, `WITH inputs AS (`+assertionIndexInputsSQL()+`) SELECT count(*),count(v.assertion_id) FILTER(WHERE v.input_hash=i.input_hash AND v.assertion_revision=i.version AND v.embedding IS NOT NULL AND vector_dims(v.embedding)=(SELECT dimension FROM memory_embedder_versions WHERE version=$1) AND vector_norm(v.embedding)>0)
 FROM inputs i LEFT JOIN memory_assertion_embedding_versions v ON v.assertion_id=i.id AND v.version=$1`, version).Scan(&total, &done)
	return
}
func (s *postgresDataStore) assertionReembedNext(ctx context.Context, version string, after int64, limit int) ([]int64, error) {
	if err := s.requireAllVectorScopes(ctx); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `WITH inputs AS (`+assertionIndexInputsSQL()+`) SELECT i.id FROM inputs i LEFT JOIN memory_assertion_embedding_versions v ON v.assertion_id=i.id AND v.version=$1
 WHERE i.id>$2 AND (v.assertion_id IS NULL OR v.input_hash<>i.input_hash OR v.assertion_revision<>i.version OR v.embedding IS NULL OR vector_dims(v.embedding)<>(SELECT dimension FROM memory_embedder_versions WHERE version=$1) OR vector_norm(v.embedding)=0) ORDER BY i.id LIMIT $3`, version, after, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	ids := []int64{}
	for rows.Next() {
		var id int64
		if err := rows.Scan(&id); err != nil {
			return nil, err
		}
		ids = append(ids, id)
	}
	return ids, rows.Err()
}
func (s *postgresDataStore) assertionReembedPoint(ctx context.Context, trace uint64, executor egress.Executor, version string, id int64) (EmbedResponse, error) {
	response := EmbedResponse{}
	err := s.vectorTransaction(ctx, func(bound *postgresDataStore) error {
		if err := bound.requireAllVectorScopes(ctx); err != nil {
			return err
		}
		var command string
		var dimension int
		if err := bound.db.QueryRow(ctx, `SELECT command,dimension FROM memory_embedder_versions WHERE version=$1`, version).Scan(&command, &dimension); err != nil {
			return err
		}
		var locked int64
		if err := bound.db.QueryRow(ctx, `SELECT id FROM entity_edges WHERE id=$1 FOR UPDATE`, id).Scan(&locked); err != nil {
			if store.IsNoRows(err) {
				return nil
			}
			return err
		}

		// Stabilize canonical memory parents before disclosing assertion text to the
		// model route. Source release still rechecks current authority independently.
		rows, err := bound.db.Query(ctx, `SELECT m.id FROM memories m WHERE EXISTS(SELECT 1 FROM fact_evidence f WHERE f.assertion_id=$1 AND f.source_kind='memory' AND f.source_id='memory:'||m.id::text AND f.invalidated_at='') ORDER BY m.id FOR SHARE`, id)
		if err != nil {
			return err
		}
		for rows.Next() {
			var parent int64
			if err := rows.Scan(&parent); err != nil {
				rows.Close()
				return err
			}
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return err
		}
		var revision int64
		var input, hash string
		if err := bound.db.QueryRow(ctx, `WITH inputs AS (`+assertionIndexInputsSQL()+`) SELECT version,input_text,input_hash FROM inputs WHERE id=$1`, id).Scan(&revision, &input, &hash); err != nil {
			if store.IsNoRows(err) {
				_, err = bound.db.Exec(ctx, `DELETE FROM memory_assertion_embedding_versions WHERE assertion_id=$1`, id)
				return err
			}
			return err
		}
		var exists bool
		if err := bound.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM memory_assertion_embedding_versions WHERE version=$1 AND assertion_id=$2 AND assertion_revision=$3 AND input_hash=$4 AND embedding IS NOT NULL AND vector_dims(embedding)=$5 AND vector_norm(embedding)>0)`, version, id, revision, hash, dimension).Scan(&exists); err != nil {
			return err
		}
		if exists {
			response.Embedded = true
			return nil
		}
		screened, err := screenMemoryText(input)
		if err != nil {
			return err
		}
		response = bound.embedForVersion(ctx, trace, executor, version, EmbedRequest{BaseURL: command, InputType: "document", Text: screened, MaxDim: dimension})
		if ctx.Err() != nil {
			return ctx.Err()
		}
		var literal any
		detail := response.Error
		if detail == "" && !response.Unavailable && !response.Unauthorized && !response.Truncated && len(response.Vector) == dimension {
			if err := bound.checkEmbeddingIdentity(ctx, version, response.ServingID); err != nil {
				detail = err.Error()
			} else {
				literal, err = vectorLiteral(response.Vector)
				if err != nil {
					detail = err.Error()
				}
			}
		} else if detail == "" {
			detail = "assertion embedding unavailable or incompatible"
		}
		if detail != "" {
			literal = nil
		}
		// Canonical assertion and all parent revisions are re-observed after model
		// work. The assertion lock does not grant authority over a changed parent.
		tag, err := bound.db.Exec(ctx, `WITH inputs AS (`+assertionIndexInputsSQL()+`)
 INSERT INTO memory_assertion_embedding_versions(version,assertion_id,assertion_revision,input_hash,embedding,attempts,last_error)
 SELECT $1,id,version,input_hash,$5::vector,1,$6 FROM inputs WHERE id=$2 AND version=$3 AND input_hash=$4
 ON CONFLICT(version,assertion_id) DO UPDATE SET assertion_revision=EXCLUDED.assertion_revision,input_hash=EXCLUDED.input_hash,
 embedding=EXCLUDED.embedding,attempts=CASE WHEN memory_assertion_embedding_versions.input_hash=EXCLUDED.input_hash THEN memory_assertion_embedding_versions.attempts+1 ELSE 1 END,
 last_error=EXCLUDED.last_error,updated_at=pg_now_text()`, version, id, revision, hash, literal, detail)
		if err != nil {
			return err
		}
		response.Vector = nil
		response.Embedded = tag.RowsAffected() == 1 && literal != nil
		response.Error = detail
		if tag.RowsAffected() == 0 {
			response.Error = "assertion input changed during indexing"
		}
		return nil
	})
	return response, err
}

func (s *postgresDataStore) indexAssertionBatch(ctx context.Context, executor egress.Executor, limit int) error {
	db, ok := s.db.(store.DB)
	if !ok || s.placement != PlacementKB {
		return errors.New("memory: assertion indexing requires shared owner transactions")
	}
	if limit < 1 || limit > 16 {
		limit = 16
	}
	for n := 0; n < limit; n++ {
		worked, err := func() (bool, error) {
			tx, err := db.Begin(ctx)
			if err != nil {
				return false, err
			}
			tx = s.auditTransaction(tx)
			defer tx.Rollback(context.Background())
			if err = sharedIndexContext(ctx, tx); err != nil {
				return false, err
			}
			bound := *s
			bound.db = tx
			version, _, _, err := bound.activeEmbeddingVersion(ctx)
			if err != nil || version == "" {
				return false, err
			}
			// Tombstones take priority; parent disappearance cascades all generations.
			if _, err = tx.Exec(ctx, `UPDATE kb_async_jobs j SET status='done',last_error='' WHERE kind='memory_assertion_index' AND status<>'done' AND NOT EXISTS(SELECT 1 FROM entity_edges e WHERE e.id=j.document_id)`); err != nil {
				return false, err
			}
			var id, generation int64
			err = tx.QueryRow(ctx, `SELECT j.document_id,j.generation FROM kb_async_jobs j JOIN entity_edges e ON e.id=j.document_id WHERE j.kind='memory_assertion_index' AND j.status IN ('pending','failed') AND j.attempts<8 AND (j.next_attempt_at='' OR j.next_attempt_at::timestamptz<=clock_timestamp()) ORDER BY j.id LIMIT 1 FOR UPDATE OF e SKIP LOCKED`).Scan(&id, &generation)
			if store.IsNoRows(err) {
				return false, tx.Commit(ctx)
			}
			if err != nil {
				return false, err
			}

			var held int64
			if err = tx.QueryRow(ctx, `SELECT generation FROM kb_async_jobs WHERE kind='memory_assertion_index' AND document_id=$1 AND generation=$2 FOR UPDATE SKIP LOCKED`, id, generation).Scan(&held); err != nil {
				if store.IsNoRows(err) {
					return false, nil
				}
				return false, err
			}
			response, err := bound.assertionReembedPoint(ctx, 0, executor, version, id)
			if err != nil {
				return false, err
			}
			if response.Error != "" {
				_, err = tx.Exec(ctx, `UPDATE kb_async_jobs SET status='failed',attempts=attempts+1,last_error=$3,next_attempt_at=(clock_timestamp()+interval '30 seconds')::text WHERE kind='memory_assertion_index' AND document_id=$1 AND generation=$2`, id, generation, textBound(response.Error, 1024))
			} else {
				_, err = tx.Exec(ctx, `UPDATE kb_async_jobs SET status='done',attempts=0,last_error='',next_attempt_at='' WHERE kind='memory_assertion_index' AND document_id=$1 AND generation=$2`, id, generation)
			}
			if err != nil {
				return false, err
			}
			return true, tx.Commit(ctx)
		}()
		if err != nil {
			return fmt.Errorf("assertion index pending: %w", err)
		}
		if !worked {
			break
		}
	}
	return nil
}
