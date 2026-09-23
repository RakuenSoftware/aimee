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
	MaxRows         int `json:"max_rows"`
	MaxContentBytes int `json:"max_content_bytes"`
}

func (r *hygienePreviewRequest) valid() bool {
	return r != nil && r.MaxRows > 0 && r.MaxRows <= 128 && r.MaxContentBytes > 0 && r.MaxContentBytes <= 32768
}

type hygieneFinding struct {
	ID                string                `json:"finding_id"`
	Type              string                `json:"type"`
	Reason            string                `json:"reason"`
	ProposedAction    string                `json:"proposed_action"`
	Uncertainty       string                `json:"uncertainty"`
	ContentCommitment string                `json:"content_commitment"`
	Targets           []MemoryRecordVersion `json:"expected_versions"`
}

type hygienePreview struct {
	Status          string                `json:"status"`
	DryRun          bool                  `json:"dry_run"`
	Detector        string                `json:"detector"`
	Scope           Scope                 `json:"scope"`
	OwnerID         string                `json:"owner_id"`
	Generation      string                `json:"collection_generation"`
	EligibilityTime string                `json:"eligibility_time"`
	Budget          hygienePreviewRequest `json:"budget"`
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

// Only this read-only detector is exposed. It does not call scheduled maintenance
// or interpret model-generated operations. Limits bound returned eligible rows
// and content bytes; physical index work is additionally deadline-bounded.
func (s *postgresDataStore) previewHygiene(ctx context.Context, scope Scope, budget *hygienePreviewRequest) (hygienePreview, error) {
	out := hygienePreview{Status: "ok", DryRun: true, Detector: hygieneDuplicatePolicy, Scope: scope, Findings: []hygieneFinding{}, Unvisited: "none_for_this_detector"}
	if s.placement != PlacementKB || !budget.valid() {
		return out, errors.New("memory: invalid hygiene preview")
	}
	out.Budget = *budget
	ctx, cancel := context.WithTimeout(ctx, 2*time.Second)
	defer cancel()
	var raw string
	err := s.db.QueryRow(ctx, `WITH head AS MATERIALIZED (
 SELECT o.owner_id::text AS owner_id,COALESCE(g.generation,0)::text AS generation
 FROM memory_collection_owner o LEFT JOIN memory_collection_generations g
 ON g.scope_type=$1 AND g.scope_value=$2
 WHERE o.id=1 AND memory_row_scope_visible($1,$2)
), selected AS MATERIALIZED (
 SELECT id,record_revision,content FROM memories
 WHERE scope_type=$1 AND scope_value=$2 AND `+currentMemorySQL("")+`
 ORDER BY id LIMIT $3+1
), bounded AS (
 SELECT *,row_number() OVER(ORDER BY id) AS ordinal,
 SUM(octet_length(content)::bigint) OVER(ORDER BY id ROWS UNBOUNDED PRECEDING) AS content_bytes
 FROM selected
)
SELECT jsonb_build_object('owner_id',h.owner_id,'generation',h.generation,
 'eligibility_time',CURRENT_TIMESTAMP::text,'rows',COALESCE((SELECT jsonb_agg(jsonb_build_object(
 'id',id::text,'revision',record_revision::text,'within_rows',ordinal<=$3,
 'within_bytes',content_bytes<=$4,'content',CASE WHEN ordinal<=$3 AND content_bytes<=$4 THEN content ELSE '' END)
 ORDER BY id) FROM bounded),'[]'::jsonb))::text FROM head h`, scope.Type, scope.Value, budget.MaxRows, budget.MaxContentBytes).Scan(&raw)
	if err != nil {
		return out, err
	}
	var selected struct {
		Owner      string `json:"owner_id"`
		Generation string `json:"generation"`
		Time       string `json:"eligibility_time"`
		Rows       []struct {
			ID          string `json:"id"`
			Revision    string `json:"revision"`
			WithinRows  bool   `json:"within_rows"`
			WithinBytes bool   `json:"within_bytes"`
			Content     string `json:"content"`
		} `json:"rows"`
	}
	if len(raw) > maxDataBody || json.Unmarshal([]byte(raw), &selected) != nil || selected.Rows == nil {
		return out, errors.New("memory: invalid hygiene snapshot")
	}
	out.OwnerID, out.Generation, out.EligibilityTime = selected.Owner, selected.Generation, selected.Time
	groups := map[string][]MemoryRecordVersion{}
	for _, row := range selected.Rows {
		if !row.WithinRows {
			out.Partial = true
			continue
		}
		out.RowsConsidered++
		if !row.WithinBytes {
			out.Partial = true
			continue
		}
		id, err := strconv.ParseInt(row.ID, 10, 64)
		version := MemoryRecordVersion{SchemaVersion: 1, OwnerID: selected.Owner, RecordID: row.ID, RecordRevision: row.Revision}
		if err != nil || !version.validFor(id) {
			return out, errors.New("memory: invalid hygiene source version")
		}
		out.RowsCompared++
		out.ContentBytes += len(row.Content)
		groups[row.Content] = append(groups[row.Content], version)
	}
	if out.Partial {
		out.Unvisited = "remaining_eligible_content_unknown"
	}
	for content, versions := range groups {
		if len(versions) < 2 {
			continue
		}
		commitment := releaseDigest(content)
		out.Findings = append(out.Findings, hygieneFinding{
			ID:   releaseDigest([]any{hygieneDuplicatePolicy, scope, commitment, versions}),
			Type: "possible_duplicate_cluster", Reason: "identical_content_in_visible_window",
			ProposedAction: "review_duplicate", Uncertainty: "candidate_only",
			ContentCommitment: commitment, Targets: versions})
	}
	sort.Slice(out.Findings, func(i, j int) bool { return out.Findings[i].ID < out.Findings[j].ID })
	return out, nil
}

func handleHygienePreview(options handlerOptions, invocation bus.ModuleInvocation, _ string, args commandArgs) ([]byte, bus.ModuleStatus) {
	invalid := func() ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", "hygiene requires dry_run=true, one explicit shared scope and bounded preview limits"))
	}
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	for key := range args {
		if key != "dry_run" && key != "scope" && key != "max_rows" && key != "max_content_bytes" {
			return invalid()
		}
	}
	var dry bool
	var scope Scope
	if json.Unmarshal(args["dry_run"], &dry) != nil || !dry || json.Unmarshal(args["scope"], &scope) != nil || scope.Type == "" || scope.Value == "" {
		return invalid()
	}
	normalized, err := normalizeScope(PlacementKB, scope)
	if err != nil || normalized != scope {
		return invalid()
	}
	budget := hygienePreviewRequest{MaxRows: 64, MaxContentBytes: 16384}
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
