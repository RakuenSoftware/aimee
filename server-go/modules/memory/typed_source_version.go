package memory

import (
	"encoding/json"
	"strconv"
)

// A source version binds one owner record observed by selection. It does not
// attest to dependency versions, current authorization, or final release. The
// kind separates assertion IDs from memory IDs in the same database owner.
type typedSourceVersion struct {
	Kind    string              `json:"record_kind"`
	Version MemoryRecordVersion `json:"version"`
}

func (h assertionHit) sourceVersion() *typedSourceVersion {
	version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: h.ownerID,
		RecordID: h.StableID, RecordRevision: strconv.Itoa(h.Version)}
	if !version.validFor(h.ID) {
		return nil
	}
	return &typedSourceVersion{Kind: "semantic_assertion", Version: version}
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
	id, err := strconv.ParseInt(ref.ID, 10, 64)
	return err == nil && ref.Source.Version.validFor(id)
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
