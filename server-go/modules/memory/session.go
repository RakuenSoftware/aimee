package memory

import (
	"context"
	"fmt"

	store "github.com/JBailes/aimee/server-go/db"
)

// FoldSession replaces a bounded L0 session with one L1 checkpoint. Content-free
// archived identities retain the original revocation handle and ingestion event;
// their version certificates are usable only as ancestry, never as live text.
func (s *postgresDataStore) FoldSession(ctx context.Context, sessionID string) (count int, summary string, err error) {
	if err = s.requireKBDomain(); err != nil {
		return 0, "", err
	}
	if db, ok := s.db.(store.DB); ok {
		tx, e := db.Begin(ctx)
		if e != nil {
			return 0, "", e
		}
		defer tx.Rollback(context.Background())
		bound := *s
		bound.db = s.auditTransaction(tx)
		count, summary, err = bound.FoldSession(ctx, sessionID)
		if err == nil {
			err = tx.Commit(ctx)
		}
		return
	}
	if _, ok := s.db.(store.Tx); !ok {
		return 0, "", fmt.Errorf("memory: session folding requires a transaction")
	}
	if _, err = s.db.Exec(ctx, `SAVEPOINT memory_fold`); err != nil {
		return
	}
	defer func() {
		if err != nil {
			_, _ = s.db.Exec(context.Background(), `ROLLBACK TO SAVEPOINT memory_fold`)
		}
		_, e := s.db.Exec(context.Background(), `RELEASE SAVEPOINT memory_fold`)
		if err == nil {
			err = e
		}
	}()
	return s.foldSession(ctx, sessionID)
}

func (s *postgresDataStore) foldSession(ctx context.Context, sessionID string) (int, string, error) {
	var sources string
	err := s.db.QueryRow(ctx, `WITH source AS MATERIALIZED (
 SELECT m.*,(`+currentMemorySQL("m.")+`) AS current FROM memories m
 WHERE source_session=$1 AND tier='L0' ORDER BY id LIMIT 65 FOR UPDATE
 ) SELECT jsonb_agg(jsonb_build_object('id',id::text,'revision',record_revision::text,
 'digest',encode(sha256(convert_to(content,'UTF8')),'hex')) ORDER BY id)::text
 FROM source HAVING count(*) BETWEEN 1 AND 64 AND bool_and(current)
 AND count(DISTINCT (scope_type,scope_value))=1 AND bool_and(`+automaticMutationSQL("")+`)`, sessionID).Scan(&sources)
	if store.IsNoRows(err) {
		return -1, "", nil
	}
	if err != nil {
		return 0, "", err
	}
	var count int
	var summary string
	err = s.db.QueryRow(ctx, `WITH source AS MATERIALIZED (
 SELECT id,content,lifecycle_state,scope_type,scope_value,provenance_category,epistemic_kind FROM memories
 WHERE id IN (SELECT (value->>'id')::bigint FROM jsonb_array_elements($2::jsonb))
), eligible AS (
 SELECT COUNT(*)::integer AS n,left(string_agg(content,'; ' ORDER BY id),2048) AS summary,
 min(scope_type) AS scope_type,min(scope_value) AS scope_value
 FROM source HAVING COUNT(*) BETWEEN 1 AND 64 AND bool_and(lifecycle_state='active')
 AND count(DISTINCT (scope_type,scope_value))=1 AND bool_and(`+automaticMutationSQL("")+`)
), checkpoint AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state,cognified_memory_kind)
 SELECT 'L1','episode','episode','session:'||$1,summary,0.8,0.8,$1,'agent_message',
 scope_type,scope_value,'active','session_checkpoint' FROM eligible RETURNING id,content,scope_type,scope_value,record_revision
), lineage AS (
 INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
 SELECT 'memory',c.id,'memory-fold-input-v1',jsonb_build_object('schema_version',1,
 'owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 'record_id',input->>'id','record_revision',input->>'revision','derived_revision',c.record_revision::text)::text,1.0
 FROM checkpoint c CROSS JOIN jsonb_array_elements($2::jsonb) input
 RETURNING 1
), evidence AS (
 INSERT INTO artifacts(id,kind,state,scope_kind,scope_id,operator_id,confidence,
 source_bundle_hash,payload)
 SELECT md5(jsonb_build_array('memory-fold'::text,$1::text,c.scope_type,c.scope_value,c.content)::text)::uuid::text,
 'session_summary','proposed',c.scope_type,c.scope_value,'kb.fold_session',1.0,
 md5(c.content),jsonb_build_object('source_kind','session_summary','scope_kind',c.scope_type,
 'scope_id',c.scope_value,'session_id',$1,'content_hash',md5(c.content),'content',c.content,
 'memory_id',c.id)
 FROM checkpoint c ON CONFLICT(id) DO UPDATE SET id=EXCLUDED.id RETURNING id
), embed_queue AS (
 INSERT INTO evidence_index_ops(artifact_id,collection) SELECT id,'evidence' FROM evidence
 ON CONFLICT DO NOTHING RETURNING 1
), synth_queue AS (
 INSERT INTO learning_synth_ops(artifact_id) SELECT id FROM evidence
 ON CONFLICT DO NOTHING RETURNING 1
) SELECT e.n,c.content FROM eligible e CROSS JOIN checkpoint c
WHERE (SELECT COUNT(*) FROM lineage)=e.n`, sessionID, sources).Scan(&count, &summary)
	if err != nil {
		return 0, "", err
	}
	// Preserve card ancestry before cascading away its payload unit. Ordinary
	// memory observations already retain their exact original output revision.
	if _, err = s.db.Exec(ctx, `INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',u.memory_id,'memory-fold-input-v1',jsonb_build_object('schema_version',1,
 'owner_id',l.source_ref::jsonb->>'owner_id','record_id',input->>'record_id',
 'record_revision',input->>'record_revision','derived_revision',original->>'revision')::text
 FROM jsonb_array_elements($1::jsonb) original JOIN memory_units u ON u.memory_id=(original->>'id')::bigint
 JOIN memory_lineage l ON l.object_type='memory_unit' AND l.object_id=u.id AND l.source_kind='episode-card-input-v1'
 CROSS JOIN LATERAL jsonb_array_elements(l.source_ref::jsonb->'inputs') input
 WHERE u.is_episode_card=1 AND u.unit_type='episode_card'`, sources); err != nil {
		return 0, "", err
	}
	if _, err = s.db.Exec(ctx, `SELECT memory_compact_source_children((value->>'id')::bigint)
 FROM jsonb_array_elements($1::jsonb)`, sources); err != nil {
		return 0, "", err
	}
	if _, err = s.db.Exec(ctx, `WITH compacted AS (
 UPDATE memories SET tier='L1',lifecycle_state='archived',archive_reason='session_compacted',
 key='compacted:'||id::text,content='',use_cases='',artifact_type=NULL,artifact_ref=NULL,artifact_hash=NULL,
 contradiction_group='',negation_tokens='',content_hash='',cognified_memory_kind='compaction_origin',updated_at=pg_now_text()
 WHERE id IN(SELECT (value->>'id')::bigint FROM jsonb_array_elements($1::jsonb)) RETURNING id,record_revision
 ) INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref)
 SELECT 'memory',c.id,'memory-compaction-origin-v1',jsonb_build_object('schema_version',1,
 'owner_id',(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
 'record_revision',original->>'revision','compacted_revision',c.record_revision::text,
 'payload_digest',original->>'digest')::text FROM compacted c
 JOIN jsonb_array_elements($1::jsonb) original ON (original->>'id')::bigint=c.id`, sources); err != nil {
		return 0, "", err
	}
	return count, summary, err
}
