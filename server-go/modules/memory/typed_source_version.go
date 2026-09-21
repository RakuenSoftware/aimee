package memory

import (
	"encoding/json"
	"strconv"
)

// A source version binds a selected owner record and its direct memory parents
// from the same snapshot. It does not attest to transitive dependencies, current
// authorization, or final release. Kind separates assertion IDs from memory IDs.
type typedSourceVersion struct {
	Kind              string                `json:"record_kind"`
	Version           MemoryRecordVersion   `json:"version"`
	MemoryParents     []MemoryRecordVersion `json:"memory_parents,omitempty"`
	MemoryParentState string                `json:"memory_parent_state,omitempty"`
}

const maxTypedMemoryParents = 64

func (h assertionHit) sourceVersion() *typedSourceVersion {
	version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: h.ownerID,
		RecordID: h.StableID, RecordRevision: strconv.Itoa(h.Version)}
	if !version.validFor(h.ID) {
		return nil
	}
	state := "unavailable"
	if h.memoryParentsObserved {
		state = "observed"
	}
	return &typedSourceVersion{Kind: "semantic_assertion", Version: version, MemoryParents: h.memoryParents, MemoryParentState: state}
}

func validTypedSource(ref typedProjectionRef) bool {
	if ref.Source == nil {
		// Older projections and channels without owner version contracts cannot
		// claim source-version evidence. Their byte commitments still apply.
		return true
	}
	if ref.Channel != "current_assertions" && ref.Channel != "historical_assertions" || ref.Source.Kind != "semantic_assertion" {
		return false
	}
	switch ref.Source.MemoryParentState {
	case "observed":
	case "", "unavailable":
		if len(ref.Source.MemoryParents) != 0 {
			return false
		}
	default:
		return false
	}
	id, err := strconv.ParseInt(ref.ID, 10, 64)
	if err != nil || !ref.Source.Version.validFor(id) || len(ref.Source.MemoryParents) > maxTypedMemoryParents {
		return false
	}
	var previous int64
	for _, parent := range ref.Source.MemoryParents {
		id, err := strconv.ParseInt(parent.RecordID, 10, 64)
		if err != nil || id <= previous || parent.OwnerID != ref.Source.Version.OwnerID || !parent.validFor(id) {
			return false
		}
		previous = id
	}
	return true
}

func validTypedSourceItem(ref typedProjectionRef, raw json.RawMessage) bool {
	if !validTypedSource(ref) {
		return false
	}
	if ref.Source == nil {
		return true
	}
	var assertion struct {
		ID       json.Number `json:"assertion_id"`
		Version  json.Number `json:"version"`
		StableID string      `json:"stable_id"`
	}
	return json.Unmarshal(raw, &assertion) == nil && assertion.ID.String() == ref.ID &&
		assertion.StableID == ref.ID && assertion.Version.String() == ref.Source.Version.RecordRevision
}

func typedSourceVersionState(refs []typedProjectionRef) string {
	observed := 0
	for _, ref := range refs {
		if ref.Source != nil {
			observed++
		}
	}
	if observed == 0 {
		return "unavailable"
	}
	if observed == len(refs) {
		return "record_versions_observed"
	}
	return "partial"
}
