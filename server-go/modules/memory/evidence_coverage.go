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
// supports exact subject/relation current-state questions only. This is evidence
// coverage at the packing boundary, not answer correctness or release authority.
type evidenceRequirementSet struct {
	Recovery      *evidenceRecoveryBudget `json:"recovery_budget,omitempty"`
	SchemaVersion int                     `json:"schema_version"`
	TaskRevision  string                  `json:"task_revision"`
	QueryMode     string                  `json:"query_mode"`
	Obligations   []evidenceObligation    `json:"obligations"`
}
type evidenceObligation struct {
	Subject  string `json:"subject"`
	Relation string `json:"relation"`
	Optional bool   `json:"optional,omitempty"`
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
	PlannerVersion    string                 `json:"planner_version"`
	TaskRevision      string                 `json:"task_revision"`
	RequirementDigest string                 `json:"requirement_digest"`
	SelectionDigest   string                 `json:"selection_digest"`
	Boundary          string                 `json:"boundary"`
	ReleaseState      string                 `json:"release_state"`
	Status            string                 `json:"status"`
	Roles             []evidenceRoleCoverage `json:"roles"`
}

func (p *evidenceRequirementSet) valid() bool {
	if p == nil || p.SchemaVersion != 1 || len(p.TaskRevision) == 0 || len(p.TaskRevision) > 128 || len(p.QueryMode) > 64 || len(p.Obligations) == 0 || len(p.Obligations) > 16 {
		return false
	}
	if p.Recovery != nil && !p.Recovery.valid() {
		return false
	}
	seen := map[[2]string]bool{}
	required := false
	for _, o := range p.Obligations {
		key := [2]string{o.Subject, o.Relation}
		if strings.TrimSpace(o.Subject) == "" || strings.TrimSpace(o.Relation) == "" || len(o.Subject) > 256 || len(o.Relation) > 128 || seen[key] {
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

// Record only owner-selected candidates. Read counters, ranking confidence and
// unversioned summaries cannot establish a role or independent support.
func coverageAssertion(item typedItem) (assertionHit, bool) {
	var h assertionHit
	raw, err := json.Marshal(item.value)
	if err != nil || json.Unmarshal(raw, &h) != nil || h.Historical || h.Object == "" || (h.Lifecycle != "persistent" && h.Lifecycle != "promoted") || item.source == nil || item.source.Kind != "semantic_assertion" || item.source.MemoryParentState != "observed" {
		return h, false
	}
	ref := typedProjectionRef{Channel: "current_assertions", ID: item.id, Source: item.source}
	if !validTypedSourceItem(ref, raw) {
		return h, false
	}
	if p := item.source.ReadPolicy; p != nil && (p.ValidAt != "" || p.BelievedAt != "") {
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
	c := &evidenceCoverage{PlannerVersion: "current-state-roles-v1", TaskRevision: p.TaskRevision,
		RequirementDigest: fmt.Sprintf("sha256:%x", sha256.Sum256(raw)), SelectionDigest: r.SelectionDigest,
		Boundary: "typed_memory_projection", ReleaseState: "not_revalidated", Status: "unknown", Roles: []evidenceRoleCoverage{}}
	r.Coverage = c
	if !p.valid() || p.QueryMode != "current_state" {
		r.Sufficiency = "unknown"
		r.Reason = "unsupported evidence requirement shape"
		return
	}
	channel := r.Channels["current_assertions"]
	satisfied, required, uncertain := 0, 0, false
	for i, o := range p.Obligations {
		role := evidenceRoleCoverage{Subject: o.Subject, Relation: o.Relation, Role: "current_state", Optional: o.Optional, Status: "missing", Retained: []string{}}
		objects := map[string]bool{}
		candidates, conflicted := 0, false
		for _, item := range r.coverageCandidates {
			h, ok := coverageAssertion(item)
			if !ok || h.Subject != o.Subject || h.Relation != o.Relation {
				continue
			}
			candidates++
			objects[h.Object] = true
			conflicted = conflicted || h.Contradiction > 0
		}
		for _, item := range channel.selected {
			h, ok := coverageAssertion(item)
			if ok && h.Subject == o.Subject && h.Relation == o.Relation {
				role.Retained = append(role.Retained, item.id)
			}
		}
		switch {
		case !channel.Enabled || channel.Status != "ok" || r.degraded || r.Status != "ok":
			role.Status = "unavailable"
		case conflicted || len(objects) > 1:
			role.Status = "conflicted"
		case len(role.Retained) > 0:
			role.Status = "satisfied"
		case candidates > 0:
			role.Status = "budget_dropped"
		}
		// Repacking cannot cure an earlier conflict or unavailable role simply by
		// omitting the offending evidence. Prior coverage can only restrict, never
		// establish satisfaction in this new selection.
		if prior := r.coveragePrior; prior != nil {
			if prior.RequirementDigest != c.RequirementDigest || len(prior.Roles) != len(p.Obligations) {
				role.Status = "unavailable"
			} else {
				old := prior.Roles[i]
				if old.Subject != o.Subject || old.Relation != o.Relation {
					role.Status = "unavailable"
				} else if old.Status != "satisfied" {
					switch old.Status {
					case "missing", "budget_dropped", "conflicted", "unavailable":
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
		}
		c.Roles = append(c.Roles, role)
	}
	switch {
	case uncertain:
		c.Status = "unknown"
	case satisfied == required:
		c.Status = "complete"
	case satisfied > 0:
		c.Status = "partial"
	default:
		c.Status = "insufficient"
	}
	r.Sufficiency = c.Status
	r.Reason = "explicit current-state obligations evaluated against retained owner-versioned assertions"
}
