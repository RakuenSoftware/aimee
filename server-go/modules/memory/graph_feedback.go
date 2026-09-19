package memory

import (
	"context"
	"encoding/json"
	"errors"
	"math"

	store "github.com/JBailes/aimee/server-go/db"
)

// Path entries are the canonical nodes and incoming relations recorded during
// retrieval. They are host-owned evidence, not a public command's authority.
type GraphPathEntry struct {
	Node     string `json:"node"`
	Relation string `json:"relation"`
	Hop      int    `json:"hop"`
}

func graphPathCredits(delta float64, path []GraphPathEntry) ([]float64, error) {
	if len(path) == 0 || len(path) > 32 || math.IsNaN(delta) || math.IsInf(delta, 0) {
		return nil, errors.New("memory: invalid feedback path")
	}
	credits := make([]float64, len(path))
	denominator := 0.0
	for i, edge := range path {
		hop := edge.Hop
		if hop <= 0 {
			hop = i + 1
		}
		gravity, ok := graphGravity[edge.Relation]
		if !ok {
			gravity = .45
		}
		credits[i] = gravity * math.Pow(.5, float64(hop-1))
		denominator += credits[i]
	}
	if denominator <= 0 {
		return nil, errors.New("memory: degenerate feedback path")
	}
	for i := range credits {
		credits[i] = delta * credits[i] / denominator
	}
	return credits, nil
}

func (s *postgresDataStore) feedbackPath(ctx context.Context, request DataRequest) error {
	if s.placement != PlacementKB {
		return errors.New("memory: graph feedback requires KB placement")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return errors.New("memory: graph feedback requires a transaction")
	}
	delta := .1
	if !request.Success {
		delta = -delta
	}
	credits, err := graphPathCredits(delta, request.GraphPath)
	if err != nil {
		return err
	}
	type nodeCredit struct {
		Node   string  `json:"node"`
		Credit float64 `json:"credit"`
	}
	nodes := make([]nodeCredit, 0, len(credits))
	for i, entry := range request.GraphPath {
		if entry.Node == "" || len(entry.Node) > 4096 {
			return errors.New("memory: invalid feedback node")
		}
		nodes = append(nodes, nodeCredit{entry.Node, credits[i]})
	}
	raw, _ := json.Marshal(nodes)
	// Preserve incident-edge feedback, summing once when both endpoints occur in
	// the path. Hidden/retired memory evidence and stale code cannot be reinforced.
	// Visibility is fixed on the transaction by the Go owner.
	_, err = s.db.Exec(ctx, `WITH nodes AS (SELECT * FROM jsonb_to_recordset($1::jsonb) AS n(node text,credit double precision)),
 visible AS MATERIALIZED (SELECT id FROM memories WHERE lifecycle_state='active' AND activation_suppressed=0),
 eligible AS (SELECT e.id,sum(n.credit) AS credit FROM entity_edges e JOIN nodes n ON n.node=e.source OR n.node=e.target
 WHERE e.edge_class<>'semantic' AND (NOT EXISTS(SELECT 1 FROM fact_evidence fe WHERE fe.assertion_id=e.id AND fe.source_kind='memory')
 OR EXISTS(SELECT 1 FROM fact_evidence fe JOIN visible v ON fe.source_id='memory:'||v.id::text WHERE fe.assertion_id=e.id AND fe.source_kind='memory'))
 AND (e.edge_origin<>'code_projection' OR EXISTS(SELECT 1 FROM code_projection_generations g JOIN projects p ON p.name=g.project
 WHERE g.id=e.projection_generation_id AND g.state='visible' AND p.lifecycle_state='current'
 AND (current_setting('aimee.memory_scope_all',true)='1' OR p.name=current_setting('aimee.memory_project',true))))
 AND (EXISTS(SELECT 1 FROM memory_entities me JOIN visible v ON v.id=me.memory_id WHERE me.entity=n.node)
 OR EXISTS(SELECT 1 FROM code_embeddings ce JOIN projects p ON p.name=ce.project
 WHERE ce.node_key=n.node AND ce.generation=p.current_generation AND p.lifecycle_state='current'
 AND (current_setting('aimee.memory_scope_all',true)='1' OR ce.project=current_setting('aimee.memory_project',true))))
 GROUP BY e.id)
 UPDATE entity_edges e SET utility_score=GREATEST(-5.0,LEAST(5.0,COALESCE(e.utility_score,0)+eligible.credit)),
 utility_touched_at=pg_now_text() FROM eligible WHERE e.id=eligible.id`, string(raw))
	return err
}

// Code vectors are only graph seeds while their project and generation are
// current. Supplying a point identifier never grants access to its project.
func (s *postgresDataStore) graphCodeSeeds(ctx context.Context, request DataRequest, exact bool) ([]string, error) {
	if len(request.CodePointIDs) > 32 {
		return nil, errors.New("memory: too many code graph seeds")
	}
	if len(request.CodePointIDs) == 0 || !graphCodeQuery(request.Query) {
		return nil, nil
	}
	rows, err := s.db.Query(ctx, `SELECT ce.node_key FROM code_embeddings ce JOIN projects p ON p.name=ce.project
 WHERE ce.point_id=ANY($1::text::bigint[]) AND p.lifecycle_state='current' AND ce.generation=p.current_generation
 AND ce.node_key<>'' AND CASE WHEN $2 THEN $3='project' AND ce.project=$4 ELSE $5 OR ce.project=$6 END
 ORDER BY array_position($1::text::bigint[],ce.point_id) LIMIT 32`, memoryIDsParameter(request.CodePointIDs), exact, request.Scope.Type, request.Scope.Value, request.IncludeAll, request.Project)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	seeds := []string{}
	for rows.Next() {
		var key string
		if err = rows.Scan(&key); err != nil {
			return nil, err
		}
		seeds = append(seeds, key)
	}
	return seeds, rows.Err()
}
