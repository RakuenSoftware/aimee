package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"sort"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

type ontologyLabel struct {
	ID    int    `json:"id"`
	Label string `json:"label"`
}
type ontologyWalkEntry struct {
	Hop          int    `json:"hop"`
	Source       string `json:"source"`
	Relation     string `json:"relation"`
	Target       string `json:"target"`
	RelationKind string `json:"relation_kind"`
	SubjectKind  string `json:"subject_kind"`
	ObjectKind   string `json:"object_kind"`
	Weight       int    `json:"weight"`
}

func ontologyLabels(names map[int]string) []ontologyLabel {
	labels := make([]ontologyLabel, 0, len(names)+1)
	for id, label := range names {
		labels = append(labels, ontologyLabel{id, label})
	}
	labels = append(labels, ontologyLabel{99, "other"})
	sort.Slice(labels, func(i, j int) bool { return labels[i].ID < labels[j].ID })
	return labels
}

func handleOntologyCommand(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	action := "list"
	if _, exists := args["action"]; exists {
		var ok bool
		action, ok = args.stringValue("action")
		if !ok {
			return commandResult(commandError("invalid_argument", "ontology action must be list or walk"))
		}
	}
	switch action {
	case "list":
		nodes, relations := ontologyLabels(ontologyNodeNames), ontologyLabels(ontologyRelationNames)
		rules := make([]map[string]string, 0, len(graphOntologyRules))
		for _, r := range ontologyRules() {
			subject, object := nodeName(r.SubjectKind), nodeName(r.ObjectKind)
			if r.SubjectKind == 99 {
				subject = "any"
			}
			if r.ObjectKind == 99 {
				object = "any"
			}
			rules = append(rules, map[string]string{"relation": relationName(r.Relation), "subject_kind": subject, "object_kind": object})
		}
		var text strings.Builder
		text.WriteString("Node kinds:\n")
		for _, n := range nodes {
			fmt.Fprintf(&text, "  %3d  %s\n", n.ID, n.Label)
		}
		text.WriteString("\nRelation kinds:\n")
		for _, r := range relations {
			fmt.Fprintf(&text, "  %3d  %s\n", r.ID, r.Label)
		}
		return commandResult(map[string]any{"status": "ok", "node_kinds": nodes, "relation_kinds": relations, "schema_rules": rules, "text": text.String()})
	case "walk":
		req := DataRequest{Operation: "ontology-walk", Entity: args.stringOr("entity", ""), Hops: 2, Limit: 128}
		if strings.TrimSpace(req.Entity) == "" || len(req.Entity) > 1024 {
			return commandResult(commandError("invalid_argument", "missing or invalid entity"))
		}
		if _, exists := args["hops"]; exists {
			n, ok := args.number("hops")
			if !ok || n < 0 || n > 128 || math.Trunc(n) != n {
				return commandResult(commandError("invalid_argument", "hops must be an integer from 0 to 128"))
			}
			req.Hops = int(n)
		}
		if raw, exists := args["relations"]; exists {
			if string(raw) == "null" || json.Unmarshal(raw, &req.Relations) != nil || len(req.Relations) > 32 {
				return commandResult(commandError("invalid_argument", "relations must be an array of at most 32 names"))
			}
			for i, name := range req.Relations {
				name = normalizeRelType(name)
				if name == "" || len(name) > 128 {
					return commandResult(commandError("invalid_argument", "invalid relation name"))
				}
				req.Relations[i] = name
			}
		}
		commandScope(args, &req)
		encoded, _ := json.Marshal(req)
		data, status := handleData(options, invocation, encoded)
		if status != bus.ModuleStatusOK {
			return commandResult(commandError("unavailable", "memory ontology walk unavailable"))
		}
		var response DataResponse
		if json.Unmarshal(data, &response) != nil || len(response.Payload) == 0 {
			return nil, bus.ModuleStatusInternal
		}
		var entries []ontologyWalkEntry
		if json.Unmarshal(response.Payload, &entries) != nil {
			return nil, bus.ModuleStatusInternal
		}
		var text strings.Builder
		for _, e := range entries {
			fmt.Fprintf(&text, "[hop %d] %s -[%s]-> %s (wt=%d)\n", e.Hop, e.Source, e.Relation, e.Target, e.Weight)
		}
		return commandResult(map[string]any{"status": "ok", "entries": entries, "text": text.String()})
	default:
		return commandResult(commandError("invalid_argument", "ontology action must be list or walk"))
	}
}

// Breadth-first traversal preserves one first-discovered edge per neighbor,
// including incoming edges, with deterministic weight/id ordering and a
// 50-edge per-node bound. Scope/currentness are applied before that bound.
func (s *postgresDataStore) ontologyWalk(ctx context.Context, req DataRequest, exact bool) ([]ontologyWalkEntry, error) {
	result := make([]ontologyWalkEntry, 0)
	frontier := []string{req.Entity}
	seen := map[string]bool{req.Entity: true}
	filters := map[string]bool{}
	for _, rel := range req.Relations {
		filters[rel] = true
	}
	for hop := 1; hop <= req.Hops && len(frontier) > 0 && len(result) < req.Limit; hop++ {
		next := []string{}
		for _, node := range frontier {
			rows, err := s.db.Query(ctx, `WITH visible AS (`+graphVisible()+`)
SELECT e.source,e.relation,e.target,COALESCE(e.relation_id,12),COALESCE(e.subject_kind,99),COALESCE(e.object_kind,99),e.weight,e.edge_class
FROM entity_edges e WHERE (e.source=$9 OR e.target=$9)
AND (e.edge_class<>'semantic' OR (e.suppressed=0 AND e.superseded_at='' AND e.invalidated_at=''
 AND e.lifecycle_state IN ('persistent','promoted')
 AND (e.valid_from='' OR e.valid_from<=pg_now_text()) AND (e.valid_until='' OR e.valid_until>pg_now_text())))
AND (NOT EXISTS(SELECT 1 FROM fact_evidence f WHERE f.assertion_id=e.id AND f.source_kind='memory')
 OR EXISTS(SELECT 1 FROM fact_evidence f JOIN visible v ON f.source_id='memory:'||v.id::text
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND f.invalidated_at='' AND f.stance='supports'))
AND (e.edge_origin<>'code_projection' OR EXISTS(SELECT 1 FROM code_projection_generations g
 JOIN projects p ON p.name=g.project WHERE g.id=e.projection_generation_id AND g.state='visible' AND p.lifecycle_state='current'
 AND CASE WHEN $1 THEN $2='project' AND p.name=$3 ELSE $4 OR p.name=$5 END))
ORDER BY e.weight DESC,e.id LIMIT 50`, append(graphScopeArgs(req, exact), node)...)
			if err != nil {
				return nil, err
			}
			for rows.Next() {
				var e ontologyWalkEntry
				var rid, subject, object int
				var class string
				if err = rows.Scan(&e.Source, &e.Relation, &e.Target, &rid, &subject, &object, &e.Weight, &class); err != nil {
					rows.Close()
					return nil, err
				}
				// Semantic relation_id points into rel_types, not the graph's legacy enum.
				if class == "semantic" {
					rid = relationCode(e.Relation)
				} else if rid < 0 || rid >= 100 {
					rid = 12
				}
				if req.Relations != nil && !filters[normalizeRelType(e.Relation)] && !filters[relationName(rid)] {
					continue
				}
				if e.Source == "" || e.Target == "" || e.Relation == "" {
					continue
				}
				neighbor := e.Source
				if neighbor == node {
					neighbor = e.Target
				}
				if seen[neighbor] {
					continue
				}
				seen[neighbor] = true
				e.Hop, e.RelationKind, e.SubjectKind, e.ObjectKind = hop, relationName(rid), nodeName(subject), nodeName(object)
				result = append(result, e)
				next = append(next, neighbor)
				if len(result) == req.Limit {
					break
				}
			}
			err = rows.Err()
			rows.Close()
			if err != nil {
				return nil, err
			}
			if len(result) == req.Limit {
				break
			}
		}
		frontier = next
	}
	return result, nil
}
