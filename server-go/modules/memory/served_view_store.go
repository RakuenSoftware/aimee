package memory

import (
	"context"
	"encoding/json"
	"errors"
	store "github.com/JBailes/aimee/server-go/db"
	"strconv"
)

func (s *postgresDataStore) servedCandidates(ctx context.Context, request DataRequest, explicit bool) ([]servedItem, []servedOmission, error) {
	view := request.ServedView.View
	items := []servedItem{}
	omissions := []servedOmission{}
	exact := Scope{}
	if explicit {
		exact = request.Scope
	}
	if view == "historical_context" {
		if s.placement != PlacementKB {
			return items, []servedOmission{{"historical_context", "historical_semantic_adapter_unavailable"}}, nil
		}
		hits, err := s.searchAssertions(ctx, 0, s.recallExecutor, request, explicit)
		if err != nil {
			return nil, nil, err
		}
		for _, h := range hits["assertions"].([]assertionHit) {
			items = append(items, servedAssertionItem(h, request))
		}
		if hits["channel_status"] != "ok" {
			omissions = append(omissions, servedOmission{"semantic_assertion", "lexical_fallback"})
		}
		return items, omissions, nil
	}
	if view == "open_contradictions" || view == "briefing" {
		if s.placement == PlacementKB {
			bundles, err := s.servedContradictions(ctx, exact)
			if err != nil {
				return nil, nil, err
			}
			items = append(items, bundles...)
		} else {
			omissions = append(omissions, servedOmission{"open_contradictions", "private_contradiction_adapter_unavailable"})
		}
	}
	if view == "open_contradictions" {
		return items, omissions, nil
	}
	if view == "reviewed_procedures" || view == "briefing" {
		if s.placement == PlacementKB {
			procedures, err := s.typedProcedures(ctx, request, exact)
			if err != nil {
				return nil, nil, err
			}
			for _, p := range procedures {
				items = append(items, servedItem{Kind: "reviewed_procedure", ID: p.id, Text: p.text, Content: map[string]any{"reviewed_content": p.value, "applicability": "scoped_reviewed_candidate", "outcome_state": "unknown"}, Sources: []typedProjectionRef{{Channel: "approved_procedures", ID: p.id, Source: p.source}}, Priority: 3})
			}
		} else {
			omissions = append(omissions, servedOmission{"reviewed_procedures", "private_reviewed_procedure_adapter_unavailable"})
		}
	}
	if view == "reviewed_procedures" {
		return items, omissions, nil
	}
	table, predicate := "memories", currentMemorySQL("m.")
	scopeSQL := `($1='' OR (m.scope_type=$1 AND m.scope_value=$2))`
	if s.placement == PlacementServer {
		table = "user_memories"
		predicate = personalCurrentMemorySQL("m.")
		scopeSQL = `($1::text='' OR $1::text='user') AND $2::text=$2::text`
	}
	kindSQL := `true`
	switch view {
	case "active_constraints":
		kindSQL = `m.kind IN ('constraint','instruction','policy')`
	case "current_state":
		kindSQL = `m.kind IN ('fact','state','task_state','constraint')`
	case "recent_decisions":
		kindSQL = `m.kind='decision'`
	case "known_failures":
		kindSQL = `m.kind IN ('failure','anti_pattern','counterexample')`
	}
	rows, err := s.db.Query(ctx, `SELECT m.id FROM `+table+` m WHERE `+predicate+` AND `+scopeSQL+` AND `+kindSQL+`
 AND (m.kind IN ('constraint','instruction','policy') OR to_tsvector('simple',m.key||' '||m.content) @@ plainto_tsquery('simple',$3))
 ORDER BY CASE WHEN m.kind IN ('constraint','instruction','policy') THEN 0 ELSE 1 END,
 CASE WHEN m.provenance_category='user_stated' THEN 0 ELSE 1 END,m.updated_at DESC,m.id DESC LIMIT 65`, exact.Type, exact.Value, request.Query)
	if err != nil {
		return nil, nil, err
	}
	ids := []int64{}
	for rows.Next() {
		var id int64
		if err = rows.Scan(&id); err != nil {
			rows.Close()
			return nil, nil, err
		}
		ids = append(ids, id)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, nil, err
	}
	if len(ids) > 64 {
		ids = ids[:64]
		omissions = append(omissions, servedOmission{"memory", "candidate_cap"})
	}
	for _, id := range ids {
		r, err := s.getAtVersioned(ctx, request.Scope, id, false, "", true)
		if errors.Is(err, ErrMemoryNotFound) {
			continue
		}
		if err != nil {
			return nil, nil, err
		}
		priority := 4
		switch r.Kind {
		case "constraint", "instruction", "policy":
			priority = 0
		case "fact", "state", "task_state":
			priority = 2
		case "decision":
			priority = 3
		}
		item := servedRecordItem(r, priority)
		if priority == 0 {
			card, err := s.claimCard(ctx, id)
			if err != nil {
				return nil, nil, err
			}
			item.Content = map[string]any{"record": r, "authority": card["authority"], "authorship": card["authorship"], "applicability": "current_scoped_constraint"}
		}
		if view == "known_failures" {
			evidence, err := s.memoryEvidence(ctx, id)
			if err != nil {
				return nil, nil, err
			}
			families, ok := evidence["source_family_count"].(int)
			if evidence["lineage_state"] != "complete" || !ok || families < 1 {
				omissions = append(omissions, servedOmission{"known_failure", "evidence_origin_unavailable"})
				continue
			}
			item.Content = map[string]any{"record": r, "evidence": evidence, "applicability": "current_scoped_task_match", "causal_generalization": "unknown"}
		}
		if view == "current_state" {
			card, err := s.claimCard(ctx, id)
			if err != nil {
				return nil, nil, err
			}
			item.Content = map[string]any{"record": r, "contradictions": card["contradictions"], "contradiction_state": card["contradiction_state"], "contradiction_selection": card["contradiction_selection"]}
		}
		items = append(items, item)
	}
	if s.placement == PlacementKB && (view == "briefing" || view == "current_state" || view == "relevant_context") {
		hits, err := s.searchAssertions(ctx, 0, s.recallExecutor, request, explicit)
		if err != nil {
			return nil, nil, err
		}
		for _, h := range hits["assertions"].([]assertionHit) {
			items = append(items, servedAssertionItem(h, request))
		}
		if hits["channel_status"] != "ok" {
			omissions = append(omissions, servedOmission{"semantic_assertion", "lexical_fallback"})
		}
	}
	return items, omissions, nil
}

func servedAssertionItem(h assertionHit, request DataRequest) servedItem {
	source := h.sourceVersion()
	if source != nil {
		source.ReadPolicy = &sourceReadPolicy{ValidAt: request.Assertions.ValidAt, BelievedAt: request.Assertions.BelievedAt, Historical: request.Assertions.Historical}
	}
	historical := h.Historical || request.Assertions.Historical
	channel := "current_assertions"
	if historical {
		channel = "historical_assertions"
	}
	return servedItem{Kind: "semantic_assertion", ID: h.StableID, Content: h, Text: h.Rendered, Sources: []typedProjectionRef{{Channel: channel, ID: h.StableID, Source: source}}, Priority: 2, Historical: historical, coverage: []typedItem{{value: h, id: h.StableID, text: h.Rendered, source: source}}}
}

func (s *postgresDataStore) servedContradictions(ctx context.Context, exact Scope, claim ...int64) ([]servedItem, error) {
	target := int64(0)
	if len(claim) > 0 {
		target = claim[0]
	}
	rows, err := s.db.Query(ctx, `SELECT c.id,c.memory_a,c.memory_b FROM memory_conflicts c
 JOIN memories a ON a.id=c.memory_a JOIN memories b ON b.id=c.memory_b
 WHERE c.resolved=0 AND `+currentMemorySQL("a.")+` AND `+currentMemorySQL("b.")+`
 AND ($1='' OR (a.scope_type=$1 AND a.scope_value=$2 AND b.scope_type=$1 AND b.scope_value=$2))
 AND ($3::bigint=0 OR c.memory_a=$3 OR c.memory_b=$3)
 ORDER BY c.detected_at DESC,c.id DESC LIMIT 64`, exact.Type, exact.Value, target)
	if err != nil {
		return nil, err
	}
	triples := [][3]int64{}
	for rows.Next() {
		var t [3]int64
		if err = rows.Scan(&t[0], &t[1], &t[2]); err != nil {
			rows.Close()
			return nil, err
		}
		triples = append(triples, t)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}
	out := []servedItem{}
	for _, t := range triples {
		a, err := s.getAtVersioned(ctx, exact, t[1], false, "", true)
		if errors.Is(err, ErrMemoryNotFound) {
			continue
		}
		if err != nil {
			return nil, err
		}
		b, err := s.getAtVersioned(ctx, exact, t[2], false, "", true)
		if errors.Is(err, ErrMemoryNotFound) {
			continue
		}
		if err != nil {
			return nil, err
		}
		ai, bi := servedRecordItem(a, 1), servedRecordItem(b, 1)
		out = append(out, servedItem{Kind: "contradiction", ID: strconv.FormatInt(t[0], 10), Text: "Unresolved contradiction\nA: " + a.Content + "\nB: " + b.Content, Content: map[string]any{"sides": []Record{a, b}, "reason": "canonical_conflict_unresolved", "valid_at": "current_owner_clock"}, Sources: append(ai.Sources, bi.Sources...), Priority: 1})
	}
	return out, nil
}

// Cards have no independent persistence or write operation.
func (s *postgresDataStore) claimCard(ctx context.Context, id int64, exact ...Scope) (map[string]any, error) {
	scope := Scope{}
	if s.placement == PlacementServer {
		scope = Scope{Type: ScopeUser}
	}
	r, err := s.getAtVersioned(ctx, scope, id, false, "", true)
	if errors.Is(err, ErrMemoryNotFound) {
		return commandError("not_found", "claim unavailable"), nil
	}
	if err != nil {
		return nil, err
	}
	if len(exact) > 0 && exact[0].Type != "" && r.Scope != exact[0] && s.placement == PlacementKB {
		return commandError("not_found", "claim unavailable"), nil
	}
	evidence, err := s.memoryEvidence(ctx, id)
	if err != nil {
		return nil, err
	}
	collection, err := s.observeRecallCollection(ctx)
	if err != nil {
		return nil, err
	}
	metadata := map[string]any{}
	var raw string
	if s.placement == PlacementKB {
		err = s.db.QueryRow(ctx, `SELECT jsonb_build_object('provenance',m.provenance_category,'lifecycle',m.lifecycle_state,'valid_from',m.valid_from,'valid_until',m.valid_until,'belief_time_state','not_recorded','actor',a.actor_principal,'actor_role',a.actor_role,'authority_rank',a.authority_rank,'calibration_state','unknown')::text FROM memories m LEFT JOIN memory_fact_actors a ON a.memory_id=m.id WHERE m.id=$1`, id).Scan(&raw)
	} else {
		err = s.db.QueryRow(ctx, `SELECT jsonb_build_object('provenance',provenance_category,'lifecycle',lifecycle_state,'valid_until',valid_until,'belief_time_state','not_recorded','actor',author_principal,'reviewer',reviewer_principal,'authority_rank',NULL,'calibration_state','unknown')::text FROM user_memories WHERE id=$1`, id).Scan(&raw)
	}
	if store.IsNoRows(err) {
		return commandError("not_found", "claim unavailable"), nil
	}
	if err != nil {
		return nil, err
	}
	if err = json.Unmarshal([]byte(raw), &metadata); err != nil {
		return nil, err
	}
	result := map[string]any{"status": "ok", "schema_version": 1, "projection_policy": servedViewPolicy, "claim": r, "source_version": r.Version, "authorship": metadata, "authority": metadata["authority_rank"], "confidence": map[string]any{"signal": r.Confidence, "calibration_state": "unknown"}, "evidence": evidence, "freshness": collection, "correction": map[string]any{"command": "memory.supersede", "old_id": strconv.FormatInt(id, 10), "expected_version": r.Version, "authority_gate": "existing_owner_mutation", "effect": "canonical_replacement_or_review_proposal"}}
	if s.placement == PlacementKB {
		conflicts, err := s.servedContradictions(ctx, Scope{}, id)
		if err != nil {
			return nil, err
		}
		related := []servedItem{}
		for _, conflict := range conflicts {
			for _, source := range conflict.Sources {
				if source.ID == strconv.FormatInt(id, 10) {
					related = append(related, conflict)
					break
				}
			}
		}
		result["contradictions"] = related
		result["contradiction_selection"] = map[string]any{"policy": "current_visible_sides", "max_bundles": 64, "bounded": len(related) == 64}
	} else {
		result["contradiction_state"] = "adapter_unavailable"
	}
	result["store"] = map[bool]string{true: "user", false: "kb"}[s.placement == PlacementServer]
	result["revalidation"] = map[string]any{"command": "memory.validity", "id": strconv.FormatInt(id, 10), "observed_version": r.Version}
	result["digest"] = servedDigest(result)
	return result, nil
}

// Expand only exact current versions already authorized by the lineage owner.
// Missing, changed, archived or hidden text is not substituted with a stale copy.
func (s *postgresDataStore) expandClaimEvidence(ctx context.Context, card map[string]any) error {
	expanded := []Record{}
	state := "complete"
	used := 0
	evidence, ok := card["evidence"].(map[string]any)
	if !ok {
		return errors.New("memory: missing card lineage")
	}
	refs, ok := evidence["evidence"].([]map[string]string)
	if !ok || evidence["lineage_state"] == "partial" {
		state = "unavailable"
	}
	scope := Scope{}
	if s.placement == PlacementServer {
		scope = Scope{Type: ScopeUser}
	}
	for _, ref := range refs {
		if len(expanded) >= 16 {
			state = "bounded"
			break
		}
		id, err := strconv.ParseInt(ref["record_id"], 10, 64)
		if err != nil {
			return err
		}
		record, err := s.getAtVersioned(ctx, scope, id, false, "", true)
		if errors.Is(err, ErrMemoryNotFound) {
			state = "partial"
			continue
		}
		if err != nil {
			return err
		}
		if record.Version == nil || record.Version.RecordRevision != ref["record_revision"] {
			state = "partial"
			continue
		}
		raw, err := json.Marshal(record)
		if err != nil {
			return err
		}
		if used+len(raw) > 16384 {
			state = "bounded"
			break
		}
		used += len(raw)
		expanded = append(expanded, record)
	}
	card["expanded_evidence"], card["expansion_state"] = expanded, state
	delete(card, "digest")
	card["digest"] = servedDigest(card)
	return nil
}
