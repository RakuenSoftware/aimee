package memory

import (
	"context"
	"errors"
)

// SearchVisible searches the shared KB's current project, workspace, and global
// records. Explicit all-scope reads append other KB scopes; the runtime's RLS
// policy still bounds the rows that this query can see.
func (s *postgresDataStore) SearchVisible(ctx context.Context, req DataRequest) ([]Record, error) {
	if s.placement != PlacementKB {
		return nil, errors.New("visible search requires KB placement")
	}
	rows, err := s.db.Query(ctx, `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence
FROM memories
WHERE lifecycle_state='active'
 AND ($1 OR scope_type='global'
      OR (scope_type='project' AND scope_value=$2)
      OR (scope_type='workspace' AND scope_value=$3))
 AND ($4='' OR key ILIKE $5 OR content ILIKE $5 OR use_cases ILIKE $5
      OR to_tsvector('english',key || ' ' || content || ' ' || COALESCE(use_cases,''))
         @@ plainto_tsquery('english',$4))
 AND ($6='' OR kind=$6) AND ($7='' OR tier=$7)
ORDER BY CASE WHEN scope_type='project' AND scope_value=$2 THEN 1
              WHEN scope_type='workspace' AND scope_value=$3 THEN 2
              WHEN scope_type='global' THEN 3 ELSE 4 END,
 (lower(key)=lower($4)) DESC,
 ts_rank_cd(to_tsvector('english',key || ' ' || content || ' ' || COALESCE(use_cases,'')),
            plainto_tsquery('english',$4)) DESC,
 confidence DESC,updated_at DESC,id DESC LIMIT $8`,
		req.IncludeAll, req.Project, req.Workspace, req.Query, searchPattern(req.Query), req.Kind, req.Tier, req.Limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	records := make([]Record, 0)
	for rows.Next() {
		var r Record
		if err := rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence); err != nil {
			return nil, err
		}
		records = append(records, r)
	}
	return records, rows.Err()
}
