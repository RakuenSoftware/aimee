package memory

import (
	"context"
	"encoding/json"
	"errors"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

func ontologyReviewName(name string) bool {
	if len(name) == 0 || len(name) >= 64 {
		return false
	}
	for _, c := range []byte(name) {
		if !(c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '_') {
			return false
		}
	}
	return true
}

func ontologyHTTP(status int, value any) ([]byte, bus.ModuleStatus) {
	body, err := json.Marshal(value)
	if err != nil || len(body) > maxDataBody {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(map[string]any{"http_status": status, "json": string(body)})
}

func handleOntologyConsole(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	operation := args.stringOr("operation", "")
	request := DataRequest{Operation: operation, IncludeAll: true}
	if operation == "ontology-review" {
		caller := options.commandContext
		if caller == nil || !caller.Authenticated || !caller.UserAuthority || caller.Principal == "" {
			return ontologyHTTP(403, map[string]string{"error": "authenticated operator required"})
		}
		request.State, request.Relation, request.FactTarget = args.stringOr("action", ""), args.stringOr("relation", ""), args.stringOr("target", "")
		if !ontologyReviewName(request.Relation) || (request.FactTarget != "" && !ontologyReviewName(request.FactTarget)) {
			return ontologyHTTP(400, map[string]string{"error": "relation/target must be lower snake_case within REL_TYPE_NAME_MAX"})
		}
		if request.State != "approve" && request.State != "map" && request.State != "reject" {
			return ontologyHTTP(400, map[string]string{"error": "action must be approve, map, or reject"})
		}
		if request.State == "map" && request.FactTarget == "" {
			return ontologyHTTP(400, map[string]string{"error": "map action requires a target"})
		}
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		if operation == "ontology-review" {
			return ontologyHTTP(500, map[string]any{"ok": false, "action": map[string]string{"approve": "approved", "map": "mapped", "reject": "rejected"}[request.State], "relation": request.Relation})
		}
		return ontologyHTTP(503, map[string]string{"error": "typed facts unavailable"})
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return ontologyHTTP(200, response.Payload)
}

// Ontology decisions use the same reversible external-change record as the
// existing graph rollback consumer. They never rewrite the assertions themselves.
func (s *postgresDataStore) reviewOntology(ctx context.Context, actor FactActor, action, relation, target string) (map[string]any, error) {
	if s.placement != PlacementKB || actor.Rank != 40 || !validMutationActor(actor) || !ontologyReviewName(relation) || (target != "" && !ontologyReviewName(target)) {
		return nil, errors.New("memory: invalid ontology review")
	}
	if _, ok := s.db.(store.Tx); !ok {
		return nil, errors.New("memory: ontology review requires transaction")
	}
	if action != "approve" && action != "map" && action != "reject" {
		return nil, errors.New("memory: invalid ontology action")
	}
	if _, err := s.db.Exec(ctx, `SELECT pg_advisory_xact_lock(4704387788844163412)`); err != nil {
		return nil, err
	}
	if action == "map" {
		if target == "" || target == relation {
			return nil, errors.New("memory: invalid ontology map target")
		}
		var id int64
		if err := s.db.QueryRow(ctx, `SELECT id FROM rel_types WHERE rel_type=$1 FOR SHARE`, target).Scan(&id); err != nil {
			return nil, err
		}
	}
	var prior string
	if err := s.db.QueryRow(ctx, `SELECT status FROM ontology_evaluations WHERE rel_type=$1 FOR UPDATE`, relation).Scan(&prior); err != nil {
		return nil, err
	}
	// Establish the verified actor before row-level audit triggers run.
	commit, err := s.openFactCommit(ctx, actor, "ontology."+action, "")
	if err != nil {
		return nil, err
	}
	status := map[string]string{"approve": "approved", "map": "mapped", "reject": "rejected"}[action]
	relationStatus := map[string]string{"approve": "active", "map": "mapped", "reject": "rejected"}[action]
	if _, err := s.db.Exec(ctx, `UPDATE rel_types SET status=$2 WHERE rel_type=$1`, relation, relationStatus); err != nil {
		return nil, err
	}
	// Preserve the old decision semantics: only mapping changes mapped_to.
	if action == "map" {
		if _, err := s.db.Exec(ctx, `UPDATE ontology_evaluations SET status=$2,mapped_to=$3,decided_at=pg_now_text() WHERE rel_type=$1`, relation, status, target); err != nil {
			return nil, err
		}
	} else if _, err := s.db.Exec(ctx, `UPDATE ontology_evaluations SET status=$2,decided_at=pg_now_text() WHERE rel_type=$1`, relation, status); err != nil {
		return nil, err
	}
	changeAction, after := action, status
	if action == "approve" {
		changeAction, after = "promote", "active/approved"
	}
	if _, err = s.db.Exec(ctx, `INSERT INTO fact_graph_changes(commit_id,assertion_id,object_kind,object_key,action,existed_before,existed_after,before_lifecycle,after_lifecycle,diff_detail) VALUES($1,0,'relation',$2,$3,1,1,'provisional/pending',$4,'external graph transition')`, commit, relation, changeAction, after); err != nil {
		return nil, err
	}
	if err = s.closeFactObjectCommit(ctx, commit, relation); err != nil {
		return nil, err
	}
	return map[string]any{"ok": true, "action": status, "relation": relation}, nil
}

func (s *postgresDataStore) ontologyDashboard(ctx context.Context) (map[string]any, error) {
	if s.settings == nil {
		return nil, errors.New("memory: ontology configuration unavailable")
	}
	settings, err := s.settings()
	if err != nil {
		return nil, err
	}
	threshold := configNumber(settings, "kb_typed_facts_promote_threshold")
	if threshold <= 0 {
		threshold = 3
	}
	var now string
	if err = s.db.QueryRow(ctx, `SELECT pg_now_text()`).Scan(&now); err != nil {
		return nil, err
	}
	result := map[string]any{"schema": "console.typed_facts.v1", "generated_at": now, "config": map[string]any{"typed_facts_enabled": true, "auto_promote": configNumber(settings, "kb_typed_facts_auto_promote_enabled") != 0, "promote_threshold": threshold}}
	rows, err := s.db.Query(ctx, `SELECT rel_type,occurrence_count,status FROM ontology_evaluations WHERE status='pending' AND occurrence_count>=1 ORDER BY occurrence_count DESC,rel_type ASC LIMIT 32`)
	if err != nil {
		return nil, err
	}
	candidates := []map[string]any{}
	for rows.Next() {
		var relation, status string
		var count int64
		if err = rows.Scan(&relation, &count, &status); err != nil {
			break
		}
		candidates = append(candidates, map[string]any{"relation": relation, "observations": count, "ready": float64(count) >= threshold, "status": status})
	}
	if err == nil {
		err = rows.Err()
	}
	rows.Close()
	if err != nil {
		return nil, err
	}
	result["promotion_candidates"], result["candidate_count"] = candidates, len(candidates)
	assertions, err := s.factCandidates(ctx, 64)
	if err != nil {
		return nil, err
	}
	result["assertion_candidates"], result["assertion_candidate_count"] = assertions, len(assertions)
	entities, err := s.ontologyEntities(ctx)
	if err != nil {
		return nil, err
	}
	result["entities"], result["entity_count"] = entities, len(entities)
	merges, err := s.ontologyMerges(ctx)
	if err != nil {
		return nil, err
	}
	result["entity_merges"], result["entity_merge_count"] = merges, len(merges)
	return result, nil
}
func (s *postgresDataStore) ontologyEntities(ctx context.Context) ([]map[string]any, error) {
	rows, err := s.db.Query(ctx, `SELECT r.canonical_id,r.kind,r.status,r.merged_into,COALESCE((SELECT a.name FROM entity_aliases a WHERE a.canonical_id=r.canonical_id AND a.suppressed=0 ORDER BY a.is_preferred DESC,a.id LIMIT 1),'') FROM entity_registry r ORDER BY r.status='active' DESC,r.canonical_id DESC LIMIT 128`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []map[string]any{}
	for rows.Next() {
		var id, merged int64
		var kind int
		var status, name string
		if err := rows.Scan(&id, &kind, &status, &merged, &name); err != nil {
			return nil, err
		}
		out = append(out, map[string]any{"canonical_id": id, "kind": kind, "status": status, "merged_into": merged, "name": name})
	}
	return out, rows.Err()
}
func (s *postgresDataStore) ontologyMerges(ctx context.Context) ([]map[string]any, error) {
	rows, err := s.db.Query(ctx, `SELECT m.id,m.from_id,m.into_id,m.undone,COALESCE((SELECT a.name FROM entity_aliases a WHERE a.canonical_id=m.from_id AND a.suppressed=0 ORDER BY a.is_preferred DESC,a.id LIMIT 1),''),COALESCE((SELECT a.name FROM entity_aliases a WHERE a.canonical_id=m.into_id AND a.suppressed=0 ORDER BY a.is_preferred DESC,a.id LIMIT 1),''),COALESCE((SELECT ch.commit_id FROM fact_graph_changes ch WHERE ch.object_kind='entity_merge' AND ch.object_key=m.id::text AND ch.action='merge' ORDER BY ch.id DESC LIMIT 1),'') FROM entity_merges m ORDER BY m.id DESC LIMIT 64`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	out := []map[string]any{}
	for rows.Next() {
		var id, from, into int64
		var undone int
		var fromName, intoName, commit string
		if err := rows.Scan(&id, &from, &into, &undone, &fromName, &intoName, &commit); err != nil {
			return nil, err
		}
		out = append(out, map[string]any{"merge_id": id, "from_id": from, "into_id": into, "undone": undone != 0, "from_name": fromName, "into_name": intoName, "commit_id": commit})
	}
	return out, rows.Err()
}
