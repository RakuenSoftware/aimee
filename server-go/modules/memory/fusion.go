package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"sort"
	"strings"
	"time"
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

const graphNodeBudget = 192

type graphVisit struct {
	Node  string  `json:"node"`
	Score float64 `json:"score"`
	Hop   int     `json:"hop"`
}

// Repeat the parent visibility predicate at both seed and result collection.
// The store's RLS context remains an additional bound, including all-scope calls.
var graphVisible = `SELECT id,scope_type,scope_value,tier,kind,key,content,confidence FROM memories
 WHERE ` + currentMemorySQL("") + `
 AND CASE WHEN $1 THEN scope_type=$2 AND scope_value=$3
 ELSE $4 OR scope_type='global' OR (scope_type='workspace' AND scope_value='_shared') OR (scope_type='project' AND scope_value=$5)
 OR (scope_type='workspace' AND scope_value=$6) END
 AND ($7='' OR kind=$7) AND ($8='' OR tier=$8)`

func graphScopeArgs(req DataRequest, exact bool) []any {
	return []any{exact, req.Scope.Type, req.Scope.Value, req.IncludeAll, req.Project, req.Workspace, req.Kind, req.Tier}
}

// Bounded breadth-first traversal across all seeds together. Each level is one
// store read, with a per-node neighbor cap and a total visit budget. Direct seed
// attachments are evidence too; they do not require an unrelated outgoing edge.
// Every memory evidence locator must resolve inside the current request audience.
// A second visible source cannot admit an edge with hidden or expired dependencies.
func (s *postgresDataStore) expandGraph(ctx context.Context, seeds []string, code bool, req DataRequest, exact bool) ([]graphVisit, error) {
	visits := make([]graphVisit, 0, graphNodeBudget)
	seen := map[string]int{}
	for _, node := range seeds {
		if node == "" || (!code && graphCodeNode(node)) {
			continue
		}
		if _, ok := seen[node]; ok {
			continue
		}
		seen[node] = len(visits)
		visits = append(visits, graphVisit{node, graphEdgeScore("", graphCodeNode(node), 0, 1, 0, 1, ""), 0})
		if len(visits) == graphNodeBudget {
			break
		}
	}
	start, end := 0, len(visits)
	now := time.Now().UTC()
	for hop := 1; hop <= 2 && start < end && len(visits) < graphNodeBudget; hop++ {
		frontier := make([]string, 0, end-start)
		for _, v := range visits[start:end] {
			frontier = append(frontier, v.Node)
		}
		encoded, _ := json.Marshal(frontier)
		rows, err := s.db.Query(ctx, `WITH frontier AS (SELECT value AS node FROM jsonb_array_elements_text($1::jsonb))
 SELECT edge.node,edge.relation,edge.weight,edge.structural_weight,edge.utility_score,edge.utility_touched_at,edge.class
 FROM frontier f CROSS JOIN LATERAL (
 SELECT e.id,CASE WHEN e.source=f.node THEN e.target ELSE e.source END AS node,e.relation,
 e.weight,e.structural_weight,e.utility_score,e.utility_touched_at,
 CASE WHEN e.edge_class='semantic' THEN e.confidence_class ELSE '' END AS class
 FROM entity_edges e WHERE (e.source=f.node OR e.target=f.node)
 AND (e.edge_class<>'semantic' OR (e.suppressed=0 AND e.superseded_at='' AND e.invalidated_at=''
 AND e.lifecycle_state IN ('persistent','promoted')
 AND (`+memoryValiditySQL("e.")+`)))
 AND NOT EXISTS(SELECT 1 FROM fact_evidence fe LEFT JOIN memories m
 ON fe.source_id='memory:'||m.id::text AND `+currentMemorySQL("m.")+`
 AND CASE WHEN $2 THEN m.scope_type=$3 AND m.scope_value=$4 ELSE $5 OR m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared')
 OR (m.scope_type='project' AND m.scope_value=$6) OR (m.scope_type='workspace' AND m.scope_value=$7) END
 WHERE fe.assertion_id=e.id AND fe.source_kind='memory' AND m.id IS NULL)
 AND (e.edge_origin<>'code_projection' OR EXISTS(SELECT 1 FROM code_projection_generations g
 JOIN projects p ON p.name=g.project WHERE g.id=e.projection_generation_id AND g.state='visible' AND p.lifecycle_state='current'
 AND CASE WHEN $2 THEN $3='project' AND p.name=$4 ELSE $5 OR p.name=$6 END))
 ORDER BY e.weight DESC,e.id LIMIT 32
 ) edge ORDER BY f.node,edge.weight DESC,edge.id`, string(encoded), exact, req.Scope.Type, req.Scope.Value, req.IncludeAll, req.Project, req.Workspace)
		if err != nil {
			return nil, err
		}
		for rows.Next() {
			var node, relation, touched, class string
			var observed, structural int
			var utility float64
			if err = rows.Scan(&node, &relation, &observed, &structural, &utility, &touched, &class); err != nil {
				rows.Close()
				return nil, err
			}
			if node == "" || (!code && graphCodeNode(node)) {
				continue
			}
			score := graphEdgeScore(relation, graphCodeNode(node), structural, observed, graphUtility(utility, touched, now), hop, class)
			if index, ok := seen[node]; ok {
				// Equal-hop paths may carry stronger evidence. A later hop never replaces
				// the shortest path or causes a cycle to be expanded again.
				if visits[index].Hop == hop && score > visits[index].Score {
					visits[index].Score = score
				}
				continue
			}
			if len(visits) == graphNodeBudget {
				continue
			}
			seen[node] = len(visits)
			visits = append(visits, graphVisit{node, score, hop})
		}
		err = rows.Err()
		rows.Close()
		if err != nil {
			return nil, err
		}
		start, end = end, len(visits)
	}
	return visits, nil
}

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
	args := append(graphScopeArgs(req, exact), memoryIDsParameter(ids), req.Query)
	rows, err := s.db.Query(ctx, `WITH visible AS MATERIALIZED (`+graphVisible+`)
 SELECT e.entity FROM memory_entities e JOIN visible m ON m.id=e.memory_id
 WHERE m.id=ANY($9::text::bigint[]) OR lower(e.entity)=lower($10)
 GROUP BY e.entity ORDER BY bool_or(m.id=ANY($9::text::bigint[])) DESC,max(e.weight) DESC,e.entity LIMIT 32`, args...)
	if err != nil {
		return nil, err
	}
	seeds := []string{}
	for rows.Next() {
		var node string
		if err = rows.Scan(&node); err != nil {
			rows.Close()
			return nil, err
		}
		seeds = append(seeds, node)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	codeSeeds, err := s.graphCodeSeeds(ctx, req, exact)
	if err != nil {
		return nil, err
	}
	seeds = append(seeds, codeSeeds...)
	visits, err := s.expandGraph(ctx, seeds, graphCodeQuery(req.Query), req, exact)
	if err != nil {
		return nil, err
	}
	if len(visits) == 0 {
		return base, nil
	}
	encoded, _ := json.Marshal(visits)
	limit := req.Limit
	if limit <= 0 || limit > 64 {
		limit = 64
	}
	args = append(graphScopeArgs(req, exact), string(encoded), limit, memoryIDsParameter(ids))
	rows, err = s.db.Query(ctx, `WITH visible AS MATERIALIZED (`+graphVisible+`), reached AS (
 SELECT * FROM jsonb_to_recordset($9::jsonb) AS n(node text,score double precision,hop int)), ranked AS (
 SELECT e.memory_id,max(n.score) AS score,COALESCE(max(n.score) FILTER(WHERE n.node ~ '^(file|symbol|import|export|route|project):'),0) AS code
 FROM reached n JOIN memory_entities e ON e.entity=n.node GROUP BY e.memory_id)
 SELECT m.id,m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence,r.score,r.code
 FROM ranked r JOIN visible m ON m.id=r.memory_id
 ORDER BY CASE WHEN $1 THEN 0 WHEN m.scope_type='project' AND m.scope_value=$5 THEN 0
 WHEN m.scope_type='workspace' AND m.scope_value=$6 THEN 1 WHEN m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared') THEN 2 ELSE 3 END,
 r.score DESC,array_position($11::text::bigint[],m.id) NULLS LAST,m.id LIMIT $10`, args...)
	if err != nil {
		return nil, err
	}
	graph := []Record{}
	signals := map[int64]Record{}
	for rows.Next() {
		var r Record
		if err = rows.Scan(&r.ID, &r.Scope.Type, &r.Scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &r.graphScore, &r.codeProximity); err != nil {
			rows.Close()
			return nil, err
		}
		graph = append(graph, r)
		signals[r.ID] = r
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	req.lanes.add(graph, laneGraph)
	out := fusePersonal(base, graph, len(base)+len(graph))
	for i := range out {
		if signal, ok := signals[out[i].ID]; ok {
			out[i].graphScore = signal.graphScore
			out[i].codeProximity = signal.codeProximity
		}
	}
	// Project/workspace priority is a visibility contract, not a text score. RRF
	// may reorder within a scope but cannot push a global match ahead of a local one.
	if !exact {
		sort.SliceStable(out, func(i, j int) bool { return recallScopeRank(out[i], req) < recallScopeRank(out[j], req) })
	}
	if len(out) > limit {
		out = out[:limit]
	}
	return out, nil
}

func recallScopeRank(r Record, req DataRequest) int {
	switch {
	case r.Scope.Type == ScopeProject && r.Scope.Value == req.Project:
		return 0
	case r.Scope.Type == ScopeWorkspace && r.Scope.Value == req.Workspace:
		return 1
	case r.Scope.Type == ScopeGlobal || (r.Scope.Type == ScopeWorkspace && r.Scope.Value == "_shared"):
		return 2
	default:
		return 3
	}
}
