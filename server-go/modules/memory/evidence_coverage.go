package memory

import (
	"bytes"
	"crypto/sha256"
	"encoding/json"
	"fmt"
	"io"
	"strings"
	"unicode/utf8"
)

// Requirements describe answer obligations, never expected record IDs. Version 1
// supports bounded, deterministic task templates over exact subject/relation obligations. This is evidence
// coverage at the packing boundary, not answer correctness or release authority.
type evidenceRequirementSet struct {
	Recovery      *evidenceRecoveryBudget `json:"recovery_budget,omitempty"`
	SchemaVersion int                     `json:"schema_version"`
	TaskRevision  string                  `json:"task_revision"`
	QueryMode     string                  `json:"query_mode"`
	Obligations   []evidenceObligation    `json:"obligations"`
}
type evidenceObligation struct {
	Role           string `json:"role,omitempty"`
	MinIndependent int    `json:"min_independent,omitempty"`
	Subject        string `json:"subject"`
	Relation       string `json:"relation"`
	Optional       bool   `json:"optional,omitempty"`
}
type evidenceRoleCoverage struct {
	Subject  string   `json:"subject"`
	Relation string   `json:"relation"`
	Role     string   `json:"role"`
	Optional bool     `json:"optional,omitempty"`
	Status   string   `json:"status"`
	Retained []string `json:"retained_ids"`
}
type evidenceCoverage struct {
	RequirementsVersion int                    `json:"requirements_version"`
	Reasons             []string               `json:"reasons"`
	PlannerVersion      string                 `json:"planner_version"`
	TaskRevision        string                 `json:"task_revision"`
	RequirementDigest   string                 `json:"requirement_digest"`
	SelectionDigest     string                 `json:"selection_digest"`
	Boundary            string                 `json:"boundary"`
	ReleaseState        string                 `json:"release_state"`
	Status              string                 `json:"status"`
	Roles               []evidenceRoleCoverage `json:"roles"`
}

func (p *evidenceRequirementSet) valid() bool {
	if p == nil || p.SchemaVersion != 1 || len(p.TaskRevision) == 0 || len(p.TaskRevision) > 128 || len(p.QueryMode) > 64 || len(p.Obligations) == 0 || len(p.Obligations) > 16 {
		return false
	}
	if p.Recovery != nil && !p.Recovery.valid() {
		return false
	}
	seen := map[[3]string]bool{}
	required := false
	for _, o := range p.Obligations {
		key := [3]string{o.Subject, o.Relation, o.Role}
		if strings.TrimSpace(o.Subject) == "" || strings.TrimSpace(o.Relation) == "" || len(o.Subject) > 256 || len(o.Relation) > 128 || seen[key] || !validEvidenceRole(o.Role) || o.MinIndependent < 0 || o.MinIndependent > 16 || (o.MinIndependent != 0 && o.Role != "independent_support") {
			return false
		}
		seen[key] = true
		required = required || !o.Optional
	}
	return required
}

// Requirement admission must preserve the declared obligations exactly. Standard
// struct decoding accepts case aliases, duplicate keys and null scalar fields,
// any of which can silently weaken the task against which coverage is measured.
func decodeEvidenceObject(raw []byte, fields map[string]any) error {
	if !utf8.Valid(raw) {
		return fmt.Errorf("evidence requirements require UTF-8")
	}
	decoder := json.NewDecoder(bytes.NewReader(raw))
	start, err := decoder.Token()
	if err != nil || start != json.Delim('{') {
		return fmt.Errorf("evidence requirements require an object")
	}
	seen := map[string]bool{}
	for decoder.More() {
		token, err := decoder.Token()
		if err != nil {
			return err
		}
		key, ok := token.(string)
		target, known := fields[key]
		if !ok || !known || seen[key] {
			return fmt.Errorf("unknown or duplicate evidence requirement field")
		}
		seen[key] = true
		var value json.RawMessage
		if err := decoder.Decode(&value); err != nil {
			return err
		}
		if bytes.Equal(value, []byte("null")) {
			return fmt.Errorf("null evidence requirement field")
		}
		if err := json.Unmarshal(value, target); err != nil {
			return err
		}
	}
	if end, err := decoder.Token(); err != nil || end != json.Delim('}') {
		return fmt.Errorf("unterminated evidence requirements")
	}
	if err := decoder.Decode(new(any)); err != io.EOF {
		return fmt.Errorf("trailing evidence requirements data")
	}
	return nil
}
func (p *evidenceRequirementSet) UnmarshalJSON(raw []byte) error {
	if len(raw) > 16384 {
		return fmt.Errorf("evidence requirements exceed bound")
	}
	var value evidenceRequirementSet
	if err := decodeEvidenceObject(raw, map[string]any{
		"schema_version": &value.SchemaVersion, "task_revision": &value.TaskRevision,
		"query_mode": &value.QueryMode, "obligations": &value.Obligations, "recovery_budget": &value.Recovery,
	}); err != nil {
		return err
	}
	if !value.valid() {
		return fmt.Errorf("invalid evidence requirements")
	}
	*p = value
	return nil
}
func (o *evidenceObligation) UnmarshalJSON(raw []byte) error {
	var value evidenceObligation
	if err := decodeEvidenceObject(raw, map[string]any{
		"subject": &value.Subject, "relation": &value.Relation, "optional": &value.Optional,
		"role": &value.Role, "min_independent": &value.MinIndependent,
	}); err != nil {
		return err
	}
	*o = value
	return nil
}
func decodeEvidenceRequirements(raw json.RawMessage) (*evidenceRequirementSet, error) {
	var p evidenceRequirementSet
	if err := json.Unmarshal(raw, &p); err != nil {
		return nil, err
	}
	return &p, nil
}

// Record only owner-selected candidates. Read counters and confidence never
// establish a role, chronology or independent support.
func coverageAssertion(item typedItem) (assertionHit, bool) {
	return coverageAssertionAt(item, false)
}
func coverageAssertionAt(item typedItem, historical bool) (assertionHit, bool) {
	var h assertionHit
	raw, err := json.Marshal(item.value)
	if err != nil || json.Unmarshal(raw, &h) != nil || h.Historical != historical || h.Object == "" ||
		(h.Lifecycle != "persistent" && h.Lifecycle != "promoted" && !(historical && h.Lifecycle == "superseded")) ||
		item.source == nil || item.source.Kind != "semantic_assertion" || item.source.MemoryParentState != "observed" {
		return h, false
	}
	channel := "current_assertions"
	if historical {
		channel = "historical_assertions"
	}
	if !validTypedSourceItem(typedProjectionRef{Channel: channel, ID: item.id, Source: item.source}, raw) {
		return h, false
	}
	if p := item.source.ReadPolicy; p != nil && (p.ValidAt != "" || p.BelievedAt != "" || (historical && !p.Historical)) {
		return h, false
	}
	if historical && (item.source.ReadPolicy == nil || !item.source.ReadPolicy.Historical) {
		return h, false
	}
	return h, true
}
func (r *typedContextResult) evaluateCoverage() {
	defer r.planEvidenceRecovery()
	p := r.Requirements
	if p == nil {
		return
	}
	raw, _ := json.Marshal(p)
	c := &evidenceCoverage{RequirementsVersion: 1, PlannerVersion: "task-roles-v2", TaskRevision: p.TaskRevision,
		RequirementDigest: fmt.Sprintf("sha256:%x", sha256.Sum256(raw)), SelectionDigest: r.SelectionDigest,
		Boundary: "typed_memory_projection", ReleaseState: "not_revalidated", Status: "unknown", Roles: []evidenceRoleCoverage{}, Reasons: []string{}}
	r.Coverage = c
	obligations, supported := p.expandedRoles()
	if !p.valid() || !supported {
		r.Sufficiency = "unknown"
		r.Reason = "unsupported evidence requirement shape"
		c.Reasons = append(c.Reasons, "unsupported_task_shape")
		return
	}
	satisfied, required, uncertain := 0, 0, false
	for i, o := range obligations {
		role := r.evaluateEvidenceRole(o)
		// Imported verdicts only restrict a freshly reconstructed retained selection.
		// A lost conflicting or unavailable candidate can never improve coverage.
		if prior := r.coveragePrior; prior != nil {
			if prior.RequirementDigest != c.RequirementDigest || len(prior.Roles) != len(obligations) {
				role.Status = "unavailable"
			} else {
				old := prior.Roles[i]
				if old.Subject != o.Subject || old.Relation != o.Relation || old.Role != o.Role {
					role.Status = "unavailable"
				} else if old.Status != "satisfied" {
					switch old.Status {
					case "missing", "budget_dropped", "conflicted", "unavailable", "stale":
						role.Status = old.Status
					default:
						role.Status = "unavailable"
					}
				}
			}
		}
		if !o.Optional {
			required++
			if role.Status == "satisfied" {
				satisfied++
			}
			if role.Status == "unavailable" {
				uncertain = true
			}
			if role.Status != "satisfied" {
				c.Reasons = append(c.Reasons, role.Role+":"+role.Status)
			}
		}
		c.Roles = append(c.Roles, role)
	}
	switch {
	case uncertain:
		c.Status = "unknown"
	case required > 0 && satisfied == required:
		c.Status = "complete"
	case satisfied > 0:
		c.Status = "partial"
	default:
		c.Status = "insufficient"
	}
	r.Sufficiency = c.Status
	r.Reason = "declared task roles evaluated against retained owner-versioned evidence; answer correctness not assessed"
}
