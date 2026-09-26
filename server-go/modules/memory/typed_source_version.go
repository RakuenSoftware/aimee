package memory

import (
	"encoding/json"
	"strconv"
	"strings"
)

// A source version binds a selected owner record and its direct memory parents
// from the same snapshot. It does not attest to transitive dependencies, current
// authorization, or final release. Kind separates assertion, episode and memory IDs.
type typedSourceVersion struct {
	UtilityHorizonPolicyDigest string                `json:"utility_horizon_policy_digest,omitempty"`
	CollectionValidUntil       string                `json:"collection_valid_until,omitempty"`
	CollectionAudience         []Scope               `json:"collection_audience,omitempty"`
	Kind                       string                `json:"record_kind"`
	Version                    MemoryRecordVersion   `json:"version"`
	MemoryParents              []MemoryRecordVersion `json:"memory_parents,omitempty"`
	MemoryParentState          string                `json:"memory_parent_state,omitempty"`
	ReadPolicy                 *sourceReadPolicy     `json:"read_policy,omitempty"`
}

func directiveSourceParentsSQL(table string) string {
	return `COALESCE((SELECT jsonb_agg(jsonb_build_object('schema_version',1,'owner_id',o.owner_id::text,
 'record_id',m.id::text,'record_revision',m.record_revision::text) ORDER BY m.id)
 FROM memories m CROSS JOIN memory_collection_owner o WHERE o.id=1 AND m.id IN (` + table + `.memory_a_id,` + table + `.memory_b_id,` + table + `.resolution_memory_id)), '[]'::jsonb)::text`
}

// The summary payload and every directly copied relation input are observed
// together. One extra parent makes overflow explicit instead of certifying a prefix.
func relationSourceParentsSQL(alias string) string {
	return `COALESCE((SELECT jsonb_agg(jsonb_build_object('schema_version',1,'owner_id',o.owner_id::text,
 'record_id',parent.id::text,'record_revision',parent.record_revision::text) ORDER BY parent.id)
 FROM (SELECT m.id,m.record_revision FROM memories m WHERE m.id=` + alias + `.memory_id OR m.id IN (
 SELECT ((CASE WHEN dep.source_kind='memory-relation-input-v2' THEN dep.source_ref::jsonb END)->>'record_id')::bigint
 FROM memory_lineage dep WHERE dep.object_type='relation' AND dep.object_id=` + alias + `.id
 AND dep.source_kind='memory-relation-input-v2')
 ORDER BY m.id LIMIT 65) parent CROSS JOIN memory_collection_owner o WHERE o.id=1),'[]'::jsonb)::text`
}

func structuredSourceColumns(table string) string {
	parents := `'[]'::text`
	if table == "epistemic_directives" {
		parents = directiveSourceParentsSQL(table)
	}
	return `,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),` + table + `.record_revision::text` + `,` + parents
}

func structuredSource(kind, owner, revision, parents string, id int64) (*typedSourceVersion, error) {
	s := &typedSourceVersion{Kind: kind, Version: MemoryRecordVersion{SchemaVersion: 1, OwnerID: owner, RecordID: strconv.FormatInt(id, 10), RecordRevision: revision}, MemoryParentState: "observed"}
	if err := json.Unmarshal([]byte(parents), &s.MemoryParents); err != nil {
		return nil, err
	}
	return s, nil
}

// Preserve the selection's temporal contract when checking it again at release.
// Empty timestamps mean the current clock, not the original selection time.
type sourceReadPolicy struct {
	ValidAt          string `json:"valid_at,omitempty"`
	BelievedAt       string `json:"believed_at,omitempty"`
	Historical       bool   `json:"include_historical,omitempty"`
	RetainedReminder bool   `json:"retained_reminder,omitempty"`
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
	if p := ref.Source.ReadPolicy; p != nil {
		if ref.Source.Kind == "memory_reminder" {
			if !p.RetainedReminder || p.ValidAt != "" || p.BelievedAt != "" || p.Historical {
				return false
			}
		} else if ref.Source.Kind != "semantic_assertion" || ref.Channel == "facts" || p.RetainedReminder || !assertionTimestamp(p.ValidAt) || !assertionTimestamp(p.BelievedAt) {
			return false
		}
	}
	if ref.Source.Kind != "memory_collection" && len(ref.Source.CollectionAudience) != 0 {
		return false
	}
	if !validCollectionDeadline(ref.Source.CollectionValidUntil) ||
		(ref.Source.CollectionValidUntil != "" && ref.Source.Kind != "memory_collection" && ref.Source.Kind != "user_memory_collection") {
		return false
	}
	if digest := ref.Source.UtilityHorizonPolicyDigest; digest != "" {
		if (ref.Source.Kind != "memory_collection" && ref.Source.Kind != "user_memory_collection") || !strings.HasPrefix(digest, "sha256:") || !receiptDigestValid(strings.TrimPrefix(digest, "sha256:")) {
			return false
		}
	}
	owner := ref.Source.Version.OwnerID
	switch ref.Source.Kind {
	case "learning_observation", "memory_relation":
		channel := "observations"
		if ref.Source.Kind == "memory_relation" {
			channel = "summaries"
		}
		if ref.Channel != channel || ref.ID == "" || len(ref.ID) > 1024 || ref.Source.MemoryParentState != "observed" {
			return false
		}
		if ref.Source.Kind == "learning_observation" && len(ref.Source.MemoryParents) != 0 {
			return false
		}
		if ref.Source.Kind == "memory_relation" && len(ref.Source.MemoryParents) == 0 {
			return false
		}
	case "learning_procedure":
		if ref.Channel != "approved_procedures" || ref.Source.MemoryParentState != "observed" || len(ref.Source.MemoryParents) != 0 {
			return false
		}
	case "memory_rule", "memory_rule_collection":
		channel := "native_rules"
		if ref.Source.Kind == "memory_rule_collection" {
			channel = "native_rule_collection"
			if ref.ID != "1" {
				return false
			}
		}
		if ref.Channel != channel || ref.Source.MemoryParentState != "observed" || len(ref.Source.MemoryParents) != 0 {
			return false
		}
	case "memory_directive", "memory_reminder":
		channel := "native_directives"
		if ref.Source.Kind == "memory_reminder" {
			channel = "native_reminders"
		}
		id, err := strconv.ParseInt(ref.ID, 10, 64)
		if err != nil || ref.Channel != channel || ref.Source.MemoryParentState != "observed" || !ref.Source.Version.validFor(id) || (ref.Source.Kind == "memory_reminder" && len(ref.Source.MemoryParents) != 0) {
			return false
		}
	case "semantic_assertion":
		if ref.Channel != "current_assertions" && ref.Channel != "historical_assertions" && ref.Channel != "facts" {
			return false
		}
	case "memory_episode":
		if ref.Channel != "episodes" || ref.Source.MemoryParentState != "observed" || len(ref.Source.MemoryParents) != 1 {
			return false
		}
	case "memory_collection", "user_memory_collection":
		if ref.Channel != "native_memory_collection" || ref.ID != "1" || len(ref.Source.MemoryParents) != 0 || ref.Source.MemoryParentState != "observed" {
			return false
		}
		if ref.Source.Kind == "memory_collection" && !validCollectionAudience(ref.Source.CollectionAudience) {
			return false
		}
	case "user_memory_record":
		if (ref.Channel != "native_identity" && ref.Channel != "native_preferences" && ref.Channel != "native_active_context" && ref.Channel != "native_open_commitments") || len(ref.Source.MemoryParents) != 0 || ref.Source.MemoryParentState != "observed" {
			return false
		}
	case "memory_record":
		if (ref.Channel != "memory_previews" && ref.Channel != "native_identity" && ref.Channel != "native_preferences" && ref.Channel != "native_active_context" && ref.Channel != "native_open_commitments") || len(ref.Source.MemoryParents) != 0 {
			return false
		}
	case "memory_summary":
		if ref.Channel != "memory_previews" || ref.Source.MemoryParentState != "observed" || len(ref.Source.MemoryParents) != 1 {
			return false
		}
	default:
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
	recordID := ref.ID
	if ref.Source.Kind == "learning_observation" || ref.Source.Kind == "memory_relation" {
		recordID = ref.Source.Version.RecordID
	}
	id, err := strconv.ParseInt(recordID, 10, 64)
	if err != nil || !ref.Source.Version.validFor(id) || len(ref.Source.MemoryParents) > maxTypedMemoryParents {
		return false
	}
	var previous int64
	for _, parent := range ref.Source.MemoryParents {
		id, err := strconv.ParseInt(parent.RecordID, 10, 64)
		if err != nil || id <= previous || parent.OwnerID != owner || !parent.validFor(id) {
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
	switch ref.Source.Kind {
	case "learning_observation":
		var item struct {
			ID string `json:"observation_id"`
		}
		return json.Unmarshal(raw, &item) == nil && item.ID == ref.ID
	case "learning_procedure":
		var item struct {
			ID json.Number `json:"proposal_id"`
		}
		return json.Unmarshal(raw, &item) == nil && item.ID.String() == ref.ID
	case "memory_relation":
		var item struct {
			Entity string `json:"entity"`
		}
		return json.Unmarshal(raw, &item) == nil && item.Entity == ref.ID
	}
	if ref.Source.Kind == "memory_episode" {
		var episode struct {
			StableID string `json:"stable_id"`
		}
		return json.Unmarshal(raw, &episode) == nil && episode.StableID == ref.ID
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
