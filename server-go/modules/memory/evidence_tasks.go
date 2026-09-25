package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"time"
)

func validEvidenceRole(role string) bool {
	switch role {
	case "", "current_state", "historical_predecessor", "temporal_anchor", "claim_support", "active_constraint", "independent_support", "approved_procedure", "source_group", "counterexample":
		return true
	}
	return false
}

// Deterministic templates expand obligations, never infer gold record identities.
// Unknown shapes deliberately remain unassessed. A temporal task needs a current
// state, its canonical predecessor and a stated world-time boundary; a procedure
// task needs both the reviewed procedure and its explicit active constraint.
func (p *evidenceRequirementSet) expandedRoles() ([]evidenceObligation, bool) {
	var roles []evidenceObligation
	switch p.QueryMode {
	case "current_state", "comparison":
		if p.QueryMode == "comparison" {
			subjects := map[string]bool{}
			for _, o := range p.Obligations {
				if !o.Optional {
					subjects[o.Subject] = true
				}
			}
			if len(subjects) < 2 {
				return nil, false
			}
		}
		for _, o := range p.Obligations {
			if o.Role == "" {
				o.Role = "current_state"
			}
			roles = append(roles, o)
		}
	case "temporal_change", "procedure_application":
		for _, o := range p.Obligations {
			if o.Role != "" {
				roles = append(roles, o)
				continue
			}
			names := []string{"current_state", "historical_predecessor", "temporal_anchor"}
			if p.QueryMode == "procedure_application" {
				names = []string{"approved_procedure", "active_constraint"}
			}
			for _, name := range names {
				copy := o
				copy.Role = name
				roles = append(roles, copy)
			}
		}
	default:
		return nil, false
	}
	if len(roles) > 48 {
		return nil, false
	}
	seen := map[[3]string]bool{}
	for _, o := range roles {
		key := [3]string{o.Subject, o.Relation, o.Role}
		if seen[key] {
			return nil, false
		}
		seen[key] = true
	}
	return roles, true
}

func evidenceTime(value string) (time.Time, bool) {
	for _, layout := range []string{"2006-01-02", time.RFC3339Nano, "2006-01-02 15:04:05.999999999", "2006-01-02T15:04:05.999999999"} {
		if stamp, err := time.Parse(layout, value); err == nil {
			return stamp, true
		}
	}
	return time.Time{}, false
}

// Only a canonical predecessor edge within the same claim can bind an update.
// Lexical similarity, an episode timestamp or shared provenance cannot do so.
func coherentPredecessor(current, old assertionHit) bool {
	if current.PriorVersionID != old.StableID || current.Subject != old.Subject || current.Relation != old.Relation || current.Object == old.Object || !old.Historical {
		return false
	}
	start, ok := evidenceTime(current.AssertedAt)
	if !ok {
		return false
	}
	end, ok := evidenceTime(old.SupersededAt)
	if !ok || end.After(start) {
		return false
	}
	before, ok := evidenceTime(old.AssertedAt)
	if !ok || before.After(start) {
		return false
	}
	// If both world-time bounds were stated, they must also be ordered. Belief
	// corrections alone do not invent a migration date.
	if old.ValidUntil != "" && current.ValidFrom != "" {
		end, ok = evidenceTime(old.ValidUntil)
		if !ok {
			return false
		}
		start, ok = evidenceTime(current.ValidFrom)
		if !ok || end.After(start) {
			return false
		}
	}
	return true
}

func evidenceRoleMatches(o evidenceObligation, items []typedItem) ([]string, bool) {
	retained := []string{}
	seen := map[string]bool{}
	add := func(id string) {
		if !seen[id] {
			seen[id] = true
			retained = append(retained, id)
		}
	}
	objects := map[string]bool{}
	conflict := false
	for _, item := range items {
		if o.Role == "approved_procedure" {
			var proc struct {
				Target string                     `json:"target_key"`
				State  string                     `json:"state"`
				Action map[string]json.RawMessage `json:"procedure"`
			}
			raw, _ := json.Marshal(item.value)
			if item.source != nil && item.source.Kind == "learning_procedure" && item.source.MemoryParentState == "observed" &&
				validTypedSourceItem(typedProjectionRef{Channel: "approved_procedures", ID: item.id, Source: item.source}, raw) &&
				json.Unmarshal(raw, &proc) == nil && proc.Target == o.Subject && proc.State == "committed" && len(proc.Action) > 0 {
				add(item.id)
			}
			continue
		}
		h, ok := coverageAssertion(item)
		if !ok || h.Subject != o.Subject || h.Relation != o.Relation {
			continue
		}
		if o.Role == "active_constraint" && h.Kind != "instruction" && h.Kind != "policy" {
			continue
		}
		objects[h.Object] = true
		conflict = conflict || h.Contradiction > 0
		switch o.Role {
		case "source_group":
			if h.OriginState == "established" {
				add(item.id)
			}
		case "counterexample":
			for _, e := range h.Evidence {
				if e.Stance == "contradicts" && e.SourceSpan != "" {
					add(item.id)
				}
			}
		case "current_state", "claim_support", "active_constraint":
			add(item.id)
		case "historical_predecessor", "temporal_anchor":
			for _, parent := range items {
				old, ok := coverageAssertionAt(parent, true)
				if !ok || parent.source.Version.OwnerID != item.source.Version.OwnerID || !coherentPredecessor(h, old) {
					continue
				}
				if o.Role == "temporal_anchor" {
					if _, ok := evidenceTime(h.ValidFrom); !ok {
						continue
					}
				}
				add(item.id)
				add(parent.id)
			}
		// MR-04's shipping owner has no independence certificate producer. Copies,
		// source counts and distinct IDs cannot stand in for unavailable certificates.
		case "independent_support":
		}
	}
	return retained, o.Role != "counterexample" && (conflict || len(objects) > 1)
}

func (r *typedContextResult) evaluateEvidenceRole(o evidenceObligation) evidenceRoleCoverage {
	role := evidenceRoleCoverage{Subject: o.Subject, Relation: o.Relation, Role: o.Role, Optional: o.Optional, Status: "missing", Retained: []string{}}
	channels := []string{"current_assertions"}
	if o.Role == "historical_predecessor" || o.Role == "temporal_anchor" {
		channels = append(channels, "historical_assertions")
	}
	if o.Role == "approved_procedure" {
		channels = []string{"approved_procedures"}
	}
	var selected []typedItem
	unavailable := r.degraded || r.Status != "ok"
	for _, name := range channels {
		c := r.Channels[name]
		if c == nil || !c.Enabled || c.Status != "ok" {
			unavailable = true
		} else {
			selected = append(selected, c.selected...)
		}
	}
	candidates, conflict := evidenceRoleMatches(o, r.coverageCandidates)
	retained, retainedConflict := evidenceRoleMatches(o, selected)
	role.Retained = retained
	switch {
	case unavailable || o.Role == "independent_support":
		role.Status = "unavailable"
	case conflict || retainedConflict:
		role.Status = "conflicted"
	case len(retained) > 0:
		role.Status = "satisfied"
	case len(candidates) > 0:
		role.Status = "budget_dropped"
	case o.Role == "source_group":
		role.Status = "unavailable"
	}
	return role
}

func (p *evidenceRequirementSet) needsOriginGroups() bool {
	if p == nil {
		return false
	}
	for _, o := range p.Obligations {
		if o.Role == "source_group" {
			return true
		}
	}
	return false
}

// Origin diagnostics come from MR-04's scoped canonical lineage owner. A bounded
// complete family is sufficient for a source-group role, never independence.
// No parent identities/counts are exported through this boolean diagnostic.
func (s *postgresDataStore) assertionOriginState(ctx context.Context, h assertionHit) string {
	if !h.memoryParentsObserved || len(h.memoryParents) == 0 || len(h.memoryParents) > 8 {
		return "unknown"
	}
	for _, p := range h.memoryParents {
		id, err := strconv.ParseInt(p.RecordID, 10, 64)
		if err != nil {
			return "unknown"
		}
		result, err := s.memoryEvidence(ctx, id)
		if err != nil || result["lineage_state"] != "complete" || result["unknown_origin_count"] != 0 {
			return "unknown"
		}
		n, ok := result["source_family_count"].(int)
		if !ok || n < 1 {
			return "unknown"
		}
	}
	return "established"
}
