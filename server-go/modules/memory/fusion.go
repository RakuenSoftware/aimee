package memory

import (
	"context"
	"fmt"
	"os"
	"strings"
)

// This is deployment configuration, shared by every client and request served
// by this instance. Server and KB processes inherit their own instance's value.
// Persist it in the service environment (or Compose file); restart to apply.
func instanceGraphFusion() (bool, error) {
	switch strings.ToLower(strings.TrimSpace(os.Getenv("AIMEE_GRAPH_FUSION"))) {
	case "", "on":
		return true, nil
	case "off":
		return false, nil
	default:
		return false, fmt.Errorf("AIMEE_GRAPH_FUSION must be on or off")
	}
}

func (s *postgresDataStore) graphFusionEnabled() bool { return s.fusionEnabled }

// Graph expansion is bounded and uses the same visibility predicate for seeds
// and returned memories. An off instance never issues graph queries.
func (s *postgresDataStore) fuseMemoryGraph(ctx context.Context, req DataRequest, exact bool, base []Record) ([]Record, error) {
	if !s.graphFusionEnabled() || s.placement != PlacementKB || req.Query == "" {
		return base, nil
	}
	var available bool
	if err := s.db.QueryRow(ctx, `SELECT to_regclass('memory_entities') IS NOT NULL AND to_regclass('entity_edges') IS NOT NULL`).Scan(&available); err != nil {
		return nil, err
	}
	if !available {
		return base, nil
	}
	ids := make([]int64, 0, len(base))
	for _, r := range base {
		ids = append(ids, r.ID)
	}
	rows, err := s.db.Query(ctx, `WITH RECURSIVE visible AS MATERIALIZED (
 SELECT id,scope_type,scope_value,tier,kind,key,content,confidence FROM memories
 WHERE lifecycle_state='active' AND activation_suppressed=0
 AND CASE WHEN $1 THEN scope_type=$2 AND scope_value=$3
 ELSE $4 OR scope_type='global' OR (scope_type='project' AND scope_value=$5)
 OR (scope_type='workspace' AND scope_value=$6) END
 AND ($7='' OR kind=$7) AND ($8='' OR tier=$8)
), seeds AS (
 SELECT DISTINCT me.entity FROM memory_entities me JOIN visible m ON m.id=me.memory_id
 WHERE m.id=ANY($9::bigint[]) OR lower(me.entity)=lower($10) LIMIT 32
), walk(node,hops,score,visited) AS (
 SELECT entity,0,1.0::double precision,ARRAY[entity] FROM seeds
 UNION ALL
 SELECT edge.node,w.hops+1,w.score*edge.score*0.5,w.visited||edge.node
 FROM walk w CROSS JOIN LATERAL (
 SELECT CASE WHEN e.source=w.node THEN e.target ELSE e.source END AS node,
 (CASE e.confidence_class WHEN 'A' THEN 1.0 WHEN 'B' THEN 0.75 ELSE 0.5 END)*
 (1.0+greatest(-0.5,least(2.0,e.utility_score))) AS score
 FROM entity_edges e WHERE (e.source=w.node OR e.target=w.node)
 AND e.suppressed=0 AND e.superseded_at='' AND e.invalidated_at=''
 AND e.lifecycle_state IN ('persistent','promoted')
 AND (e.valid_until='' OR e.valid_until>pg_now_text())
 AND (e.valid_from='' OR e.valid_from<=pg_now_text())
 ORDER BY score DESC,e.id LIMIT 32
 ) edge WHERE w.hops<2 AND NOT edge.node=ANY(w.visited)
), ranked AS (
 SELECT me.memory_id,max(w.score) AS score FROM walk w JOIN memory_entities me ON me.entity=w.node
 WHERE w.hops>0 GROUP BY me.memory_id
)
SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence
FROM ranked r JOIN visible m ON m.id=r.memory_id ORDER BY r.score DESC,m.confidence DESC,m.id LIMIT $11`,
		exact, req.Scope.Type, req.Scope.Value, req.IncludeAll, req.Project, req.Workspace, req.Kind, req.Tier, ids, req.Query, req.Limit)
	if err != nil {
		return nil, err
	}
	graph, err := scanRecordRows(rows)
	if err != nil {
		return nil, err
	}
	return fusePersonal(base, graph, req.Limit), nil
}
