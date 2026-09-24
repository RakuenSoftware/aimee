package memory

import (
	"context"

	store "github.com/JBailes/aimee/server-go/db"
)

// FoldSession atomically replaces a complete bounded L0 session with one L1
// checkpoint and a lineage row for every source. Any non-active source or a
// session over the hard cap refuses the entire operation. Protected kinds and
// user/unknown-origin sources require explicit review instead of automatic folding. Sources must share a
// scope; folding never publishes private content globally. The checkpoint,
// lineage, learning evidence and both consumer queues commit in one statement.
func (s *postgresDataStore) FoldSession(ctx context.Context, sessionID string) (int, string, error) {
	if err := s.requireKBDomain(); err != nil {
		return 0, "", err
	}
	var count int
	var summary string
	err := s.db.QueryRow(ctx, `WITH source AS MATERIALIZED (
 SELECT id,content,lifecycle_state,scope_type,scope_value,provenance_category,epistemic_kind FROM memories
 WHERE source_session=$1 AND tier='L0' ORDER BY id LIMIT 65 FOR UPDATE
), eligible AS (
 SELECT COUNT(*)::integer AS n,left(string_agg(content,'; ' ORDER BY id),2048) AS summary,
 min(scope_type) AS scope_type,min(scope_value) AS scope_value
 FROM source HAVING COUNT(*) BETWEEN 1 AND 64 AND bool_and(lifecycle_state='active')
 AND count(DISTINCT (scope_type,scope_value))=1 AND bool_and(`+automaticMutationSQL("")+`)
), checkpoint AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state)
 SELECT 'L1','episode','episode','session:'||$1,summary,0.8,0.8,$1,'agent_message',
 scope_type,scope_value,'active' FROM eligible RETURNING id,content,scope_type,scope_value
), lineage AS (
 INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
 SELECT 'memory',c.id,'memory','memory:'||s.id::text,1.0 FROM checkpoint c CROSS JOIN source s
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
), removed AS (
 DELETE FROM memories WHERE id IN (SELECT id FROM source) AND EXISTS(SELECT 1 FROM checkpoint)
 RETURNING 1
) SELECT e.n,c.content FROM eligible e CROSS JOIN checkpoint c
WHERE (SELECT COUNT(*) FROM lineage)=e.n AND (SELECT COUNT(*) FROM removed)=e.n`, sessionID).Scan(&count, &summary)
	if store.IsNoRows(err) {
		return -1, "", nil
	}
	return count, summary, err
}
