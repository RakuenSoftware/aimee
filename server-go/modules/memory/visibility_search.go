package memory

import (
	"context"
	"errors"
	"strconv"
)

// SearchVisible searches the shared KB's current project, workspace, and global
// records. Explicit all-scope reads append other KB scopes; the runtime's RLS
// policy still bounds the rows that this query can see.
func (s *postgresDataStore) SearchVisible(ctx context.Context, req DataRequest) ([]Record, error) {
	if s.placement != PlacementKB {
		return nil, errors.New("visible search requires KB placement")
	}
	req, err := s.planRecall(req)
	if err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT `+queryRecordColumns+`,ts_rank_cd(to_tsvector('english',key||' '||content||' '||COALESCE(use_cases,'')),plainto_tsquery('english',$4))
FROM memories
WHERE `+currentMemorySQL("")+`
 AND ($1 OR scope_type='global' OR (scope_type='workspace' AND scope_value='_shared')
      OR (scope_type='project' AND scope_value=$2)
      OR (scope_type='workspace' AND scope_value=$3))
 AND ($4='' OR key ILIKE $5 OR content ILIKE $5 OR use_cases ILIKE $5
      OR to_tsvector('english',key || ' ' || content || ' ' || COALESCE(use_cases,''))
         @@ plainto_tsquery('english',$4))
 AND ($6='' OR kind=$6) AND ($7='' OR tier=$7)
ORDER BY CASE WHEN scope_type='project' AND scope_value=$2 THEN 1
              WHEN scope_type='workspace' AND scope_value=$3 THEN 2
              WHEN scope_type='global' OR (scope_type='workspace' AND scope_value='_shared') THEN 3 ELSE 4 END,
 (lower(key)=lower($4)) DESC,
 ts_rank_cd(to_tsvector('english',key || ' ' || content || ' ' || COALESCE(use_cases,'')),
            plainto_tsquery('english',$4)) DESC,
 updated_at DESC,id DESC LIMIT $8`,
		req.IncludeAll, req.Project, req.Workspace, req.Query, searchPattern(req.Query), req.Kind, req.Tier, req.Limit)
	if err != nil {
		return nil, err
	}
	records := []Record{}
	for rows.Next() {
		r := Record{observedVersion: &MemoryRecordVersion{SchemaVersion: 1}, currentRead: true}
		var score float64
		if err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &r.observedVersion.OwnerID, &r.observedVersion.RecordRevision, &score); err != nil {
			rows.Close()
			return nil, err
		}
		r.observedVersion.RecordID = strconv.FormatInt(r.ID, 10)
		recordNativeRank(ctx, &r, "lexical", len(records)+1, score, "pg_ts_rank_cd_exact_key_scope_priority")
		records = append(records, r)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	return s.finalizeRecall(ctx, req, false, records)
}
