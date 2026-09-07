package memory

import (
	"context"

	store "github.com/JBailes/aimee/server-go/db"
)

// FoldSession atomically replaces a complete bounded L0 session with one L1
// checkpoint and a lineage row for every source. Any non-active source or a
// session over the hard cap refuses the entire operation.
func (s *postgresDataStore) FoldSession(ctx context.Context, sessionID string) (int, string, error) {
	if err := s.requireKBDomain(); err != nil {
		return 0, "", err
	}
	var count int
	var summary string
	err := s.db.QueryRow(ctx, `WITH source AS MATERIALIZED (
 SELECT id,content,lifecycle_state FROM memories WHERE source_session=$1 AND tier='L0' ORDER BY id
), eligible AS (
 SELECT COUNT(*)::integer AS n,left(string_agg(content,'; ' ORDER BY id),2048) AS summary
 FROM source HAVING COUNT(*) BETWEEN 1 AND 64 AND bool_and(lifecycle_state='active')
), checkpoint AS (
 INSERT INTO memories(tier,kind,epistemic_kind,key,content,confidence,confidence_ceiling,
 source_session,provenance_category,scope_type,scope_value,lifecycle_state)
 SELECT 'L1','episode','episode','session:'||$1,summary,0.8,0.8,$1,'agent_message',
 'global','_global','active' FROM eligible RETURNING id,content
), lineage AS (
 INSERT INTO memory_lineage(object_type,object_id,source_kind,source_ref,confidence)
 SELECT 'memory',c.id,'memory','memory:'||s.id::text,1.0 FROM checkpoint c CROSS JOIN source s
 RETURNING 1
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
