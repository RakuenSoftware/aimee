package memory

import (
	"context"
	"encoding/json"
	"errors"
	"sort"
	"strconv"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

const hygieneDuplicatePolicy = "exact-content-duplicates-v1"

type hygienePreviewRequest struct {
	DryRun          bool   `json:"dry_run"`
	Cursor          string `json:"cursor,omitempty"`
	MaxRows         int    `json:"max_rows"`
	MaxContentBytes int    `json:"max_content_bytes"`
}

func (r *hygienePreviewRequest) valid() bool {
	return r != nil && r.MaxRows > 0 && r.MaxRows <= 128 && r.MaxContentBytes > 0 && r.MaxContentBytes <= 32768
}

type hygieneFinding struct {
	ProposalID        string                `json:"proposal_id,omitempty"`
	ProposalState     string                `json:"proposal_state,omitempty"`
	ID                string                `json:"finding_id"`
	Type              string                `json:"type"`
	Reason            string                `json:"reason"`
	ProposedAction    string                `json:"proposed_action"`
	Uncertainty       string                `json:"uncertainty"`
	ContentCommitment string                `json:"content_commitment"`
	Targets           []MemoryRecordVersion `json:"expected_versions"`
}

type hygienePreview struct {
	RunID           string                `json:"run_id,omitempty"`
	JobID           string                `json:"job_id,omitempty"`
	TelemetryWrites bool                  `json:"job_telemetry_written"`
	ResumeCursor    string                `json:"resume_cursor,omitempty"`
	WindowStart     string                `json:"window_start"`
	Policy          string                `json:"policy"`
	Detectors       map[string]string     `json:"detectors"`
	Status          string                `json:"status"`
	DryRun          bool                  `json:"dry_run"`
	Detector        string                `json:"detector"`
	Scope           Scope                 `json:"scope"`
	OwnerID         string                `json:"owner_id"`
	Generation      string                `json:"collection_generation"`
	EligibilityTime string                `json:"eligibility_time"`
	Budget          hygienePreviewRequest `json:"budget"`
	RowsInspected   int                   `json:"retained_rows_inspected"`
	RowsConsidered  int                   `json:"eligible_rows_considered"`
	RowsCompared    int                   `json:"rows_compared"`
	ContentBytes    int                   `json:"content_bytes_compared"`
	Partial         bool                  `json:"partial"`
	Unvisited       string                `json:"unvisited"`
	ResumeAvailable bool                  `json:"resume_available"`
	CanonicalWrites int                   `json:"canonical_writes"`
	ProposalWrites  int                   `json:"proposal_writes"`
	Findings        []hygieneFinding      `json:"findings"`
}

// The detector does not call mutating maintenance
// or interpret model-generated operations. Limits bound returned eligible rows
// and content bytes; physical index work is additionally deadline-bounded.
func (s *postgresDataStore) previewHygiene(ctx context.Context, scope Scope, budget *hygienePreviewRequest) (hygienePreview, error) {
	out := hygienePreview{Status: "ok", DryRun: true, Policy: hygienePolicy, Detector: hygieneDuplicatePolicy, Scope: scope, Findings: []hygieneFinding{}, Unvisited: "none_for_this_detector"}
	if s.placement != PlacementKB || !budget.valid() {
		return out, errors.New("memory: invalid hygiene preview")
	}
	out.Budget = *budget
	out.DryRun = budget.DryRun
	cursor, after, cursorErr := decodeHygieneCursor(budget.Cursor, scope)
	if cursorErr != nil {
		return out, cursorErr
	}
	out.WindowStart = strconv.FormatInt(after, 10)
	out.Detectors = map[string]string{"possible_duplicate_cluster": "exact_content_scoped_index_cluster_limit_16", "possible_contradiction": "stored_unresolved_conflicts_visible_pairs", "obsolete_assertion_candidate": "expired_validity", "broken_correction_chain": "unavailable_visible_successor", "missing_dependency": "invalid_derived_input", "unreferenced_observation": "no_visible_memory_links", "over_exposed_memory": "unavailable_no_delivered_exposure_adapter", "low_trust_high_fanout": "confidence_below_half_at_least_eight_visible_links", "model_assistance": "disabled_no_model_calls"}
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	var raw string
	err := s.db.QueryRow(ctx, `WITH head AS MATERIALIZED (
 SELECT o.owner_id::text AS owner_id,COALESCE(g.generation,0)::text AS generation
 FROM memory_collection_owner o LEFT JOIN memory_collection_generations g
 ON g.scope_type=$1 AND g.scope_value=$2
 WHERE o.id=1 AND memory_row_scope_visible($1,$2)
), selected AS MATERIALIZED (
 SELECT m.id,m.record_revision,m.content, (`+currentMemorySQL("m.")+`) AS eligible,
 (NOT `+memoryUnexpiredSQL("m.")+`) AS expired,
 (m.merged_into>0 AND NOT EXISTS(SELECT 1 FROM memories successor WHERE successor.id=m.merged_into
  AND successor.scope_type=$1 AND successor.scope_value=$2 AND `+baseHistoricalMemoryInspectionSQL("successor.")+`)) AS broken,
 (NOT (`+currentDerivedMemoryInputsSQL("m.", true)+`)) AS dependency,
 (m.kind='observation' AND NOT EXISTS(SELECT 1 FROM memory_links l JOIN memories peer
  ON peer.id=CASE WHEN l.source_id=m.id THEN l.target_id ELSE l.source_id END
  WHERE (l.source_id=m.id OR l.target_id=m.id) AND peer.scope_type=$1 AND peer.scope_value=$2
  AND `+baseHistoricalMemoryInspectionSQL("peer.")+`)) AS unreferenced,
 (m.confidence<0.5 AND (SELECT count(*) FROM (SELECT 1 FROM memory_links l JOIN memories peer ON peer.id=l.source_id
  WHERE l.target_id=m.id AND peer.scope_type=$1 AND peer.scope_value=$2
  AND `+baseHistoricalMemoryInspectionSQL("peer.")+` LIMIT 8) fanout)>=8) AS low_trust,
 COALESCE((SELECT jsonb_agg(jsonb_build_object('schema_version',1,'owner_id',h.owner_id,
  'record_id',peer.id::text,'record_revision',peer.record_revision::text) ORDER BY peer.id)
  FROM (SELECT p.id,p.record_revision FROM memories p WHERE p.scope_type=$1 AND p.scope_value=$2
   AND md5(p.content)=md5(m.content) AND p.content=m.content AND `+currentMemorySQL("p.")+`
   ORDER BY p.id LIMIT 17) peer),'[]'::jsonb) AS duplicates,
 COALESCE((SELECT jsonb_agg(jsonb_build_object('schema_version',1,'owner_id',h.owner_id,
  'record_id',peer.id::text,'record_revision',peer.record_revision::text) ORDER BY peer.id)
  FROM (SELECT DISTINCT p.id,p.record_revision FROM memory_conflicts c JOIN memories p
   ON p.id=CASE WHEN c.memory_a=m.id THEN c.memory_b ELSE c.memory_a END
   WHERE c.resolved=0 AND (c.memory_a=m.id OR c.memory_b=m.id) AND p.id>m.id
   AND p.scope_type=$1 AND p.scope_value=$2 AND `+currentMemorySQL("p.")+` ORDER BY p.id LIMIT 8) peer),'[]'::jsonb) AS conflicts
 FROM memories m CROSS JOIN head h
 WHERE m.id>$5 AND m.scope_type=$1 AND m.scope_value=$2 AND `+baseHistoricalMemoryInspectionSQL("m.")+`
 ORDER BY m.id LIMIT $3+1
), bounded AS (
 SELECT *,row_number() OVER(ORDER BY id) AS ordinal,
 SUM(CASE WHEN eligible THEN octet_length(content)::bigint ELSE 0 END) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS content_bytes
 FROM selected
)
SELECT jsonb_build_object('owner_id',h.owner_id,'generation',h.generation,
 'eligibility_time',CURRENT_TIMESTAMP::text,'rows',COALESCE((SELECT jsonb_agg(jsonb_build_object(
 'id',id::text,'revision',record_revision::text,'eligible',eligible,'expired',expired,'broken',broken,
 'dependency',dependency,'unreferenced',unreferenced,'low_trust',low_trust,'conflicts',conflicts,'duplicates',duplicates,'within_rows',ordinal<=$3,
 'within_bytes',content_bytes<=$4,'content',CASE WHEN eligible AND ordinal<=$3 AND content_bytes<=$4 THEN content ELSE '' END)
 ORDER BY id) FROM bounded),'[]'::jsonb))::text FROM head h`, scope.Type, scope.Value, budget.MaxRows, budget.MaxContentBytes, after).Scan(&raw)
	if err != nil {
		return out, err
	}
	var selected struct {
		Owner      string `json:"owner_id"`
		Generation string `json:"generation"`
		Time       string `json:"eligibility_time"`
		Rows       []struct {
			ID           string                `json:"id"`
			Eligible     bool                  `json:"eligible"`
			Expired      bool                  `json:"expired"`
			Broken       bool                  `json:"broken"`
			Dependency   bool                  `json:"dependency"`
			Unreferenced bool                  `json:"unreferenced"`
			LowTrust     bool                  `json:"low_trust"`
			Duplicates   []MemoryRecordVersion `json:"duplicates"`
			Conflicts    []MemoryRecordVersion `json:"conflicts"`
			Revision     string                `json:"revision"`
			WithinRows   bool                  `json:"within_rows"`
			WithinBytes  bool                  `json:"within_bytes"`
			Content      string                `json:"content"`
		} `json:"rows"`
	}
	if len(raw) > maxDataBody || json.Unmarshal([]byte(raw), &selected) != nil || selected.Rows == nil {
		return out, errors.New("memory: invalid hygiene snapshot")
	}
	out.OwnerID, out.Generation, out.EligibilityTime = selected.Owner, selected.Generation, selected.Time
	if budget.Cursor != "" && (cursor.Owner != selected.Owner || cursor.Generation != selected.Generation) {
		return out, errors.New("memory: hygiene snapshot changed; restart scan")
	}
	last := after
	remaining := false
	clusterLimit := cursor.ClusterLimit
	groups := map[string][]MemoryRecordVersion{}
	for _, row := range selected.Rows {
		if !row.WithinRows {
			remaining = true
			out.Partial = true
			continue
		}
		out.RowsInspected++
		if row.Eligible {
			out.RowsConsidered++
		}
		if !row.WithinBytes {
			remaining = true
			out.Partial = true
			continue
		}
		id, err := strconv.ParseInt(row.ID, 10, 64)
		version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: selected.Owner, RecordID: row.ID, RecordRevision: row.Revision}
		if err != nil || !version.validFor(id) {
			return out, errors.New("memory: invalid hygiene source version")
		}
		last = id
		if row.Eligible {
			out.RowsCompared++
			out.ContentBytes += len(row.Content)
			peers := row.Duplicates
			if len(peers) > 16 {
				peers = peers[:16]
				clusterLimit = true
				out.Partial = true
				out.Unvisited = "duplicate_cluster_exceeds_16"
			}
			groups[row.Content] = peers
		}
		add := func(kind, reason, action string, targets []MemoryRecordVersion) {
			out.Findings = append(out.Findings, hygieneFinding{ID: releaseDigest([]any{hygienePolicy, scope, kind, reason, action, targets}), Type: kind, Reason: reason, ProposedAction: action, Uncertainty: "candidate_only_scoped_evidence", Targets: targets})
		}
		targets := []MemoryRecordVersion{version}
		if row.Expired {
			add("obsolete_assertion_candidate", "validity_interval_expired_not_deletion_authority", "review_lifecycle", targets)
		}
		if row.Broken {
			add("broken_correction_chain", "successor_not_available_in_authorized_scope", "review_evidence", targets)
		}
		if row.Dependency {
			add("missing_dependency", "derived_input_proof_not_current", "review_evidence", targets)
		}
		if row.Unreferenced {
			add("unreferenced_observation", "no_visible_memory_links_other_evidence_unknown", "review_evidence", targets)
		}
		if row.LowTrust {
			add("low_trust_high_fanout", "low_confidence_eight_or_more_visible_dependents", "review_evidence", targets)
		}
		if row.Eligible {
			for _, peer := range row.Conflicts {
				add("possible_contradiction", "stored_unresolved_conflict_requires_review", "review_evidence", []MemoryRecordVersion{version, peer})
			}
		}
	}
	if clusterLimit {
		out.Partial = true
		out.Unvisited = "duplicate_cluster_exceeds_16"
	}
	if out.Partial {
		if out.Unvisited != "duplicate_cluster_exceeds_16" {
			out.Unvisited = "remaining_retained_rows_or_content_unknown"
		}
		if remaining && last > after {
			out.ResumeAvailable = true
			out.ResumeCursor = encodeHygieneCursor(selected.Owner, scope, selected.Generation, last, clusterLimit)
		}
	}
	for content, versions := range groups {
		if len(versions) < 2 {
			continue
		}
		commitment := releaseDigest(content)
		out.Findings = append(out.Findings, hygieneFinding{
			ID:   releaseDigest([]any{hygienePolicy, hygieneDuplicatePolicy, scope, commitment, versions}),
			Type: "possible_duplicate_cluster", Reason: "identical_content_in_authorized_scope",
			ProposedAction: "review_duplicate", Uncertainty: "candidate_only",
			ContentCommitment: commitment, Targets: versions})
	}
	sort.Slice(out.Findings, func(i, j int) bool { return out.Findings[i].ID < out.Findings[j].ID })
	return out, nil
}

func handleHygienePreview(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func() ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", "hygiene requires one explicit shared scope and bounded proposal-only limits"))
	}
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	var ok bool
	args, ok = commandDomainArgs(args, "memory.hygiene")
	if !ok {
		return invalid()
	}
	for key := range args {
		if key != "dry_run" && key != "scope" && key != "max_rows" && key != "max_content_bytes" && key != "cursor" {
			return invalid()
		}
	}
	var dry bool
	var scope Scope
	if raw, exists := args["dry_run"]; exists {
		var value *bool
		if json.Unmarshal(raw, &value) != nil || value == nil {
			return invalid()
		}
		dry = *value
	}
	if json.Unmarshal(args["scope"], &scope) != nil || scope.Type == "" || scope.Value == "" {
		return invalid()
	}
	normalized, err := normalizeScope(PlacementKB, scope)
	if err != nil || normalized != scope {
		return invalid()
	}
	if !dry && (invocation.PrincipalRef != 0 || !verifiedRetryCaller(options.commandContext)) {
		return commandResult(commandError("unauthorized", "hygiene proposal admission requires authenticated host context"))
	}
	budget := hygienePreviewRequest{DryRun: dry, MaxRows: 64, MaxContentBytes: 16384}
	if raw, exists := args["cursor"]; exists {
		if json.Unmarshal(raw, &budget.Cursor) != nil || len(budget.Cursor) > 4096 {
			return invalid()
		}
	}
	if _, _, err := decodeHygieneCursor(budget.Cursor, scope); err != nil {
		return invalid()
	}
	for key, dest := range map[string]*int{"max_rows": &budget.MaxRows, "max_content_bytes": &budget.MaxContentBytes} {
		if raw, ok := args[key]; ok {
			var value *int
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return invalid()
			}
			*dest = *value
		}
	}
	if !budget.valid() {
		return invalid()
	}
	request := DataRequest{Operation: "hygiene-preview", Scope: scope, HygienePreview: &budget}
	raw, _ := json.Marshal(request)
	result, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return commandResult(commandError("unavailable", "hygiene preview unavailable; no coverage result"))
	}
	var response DataResponse
	if json.Unmarshal(result, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}
