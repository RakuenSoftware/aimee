package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"regexp"
	"sort"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

const assertionPointOffset int64 = 2000000000000

type assertionSearchRequest struct {
	ValidAt    string `json:"valid_at"`
	BelievedAt string `json:"believed_at"`
	Historical bool   `json:"include_historical"`
	Hops       int    `json:"max_hops"`
}
type assertionEvidence struct {
	SourceKind string `json:"source_kind"`
	SourceID   string `json:"source_id"`
	SourceSpan string `json:"source_span"`
	ObservedAt string `json:"observed_at"`
	Stance     string `json:"stance"`
}

func assertionRRFContribution(rank int) float64 {
	return 1 / float64(60+max(1, rank))
}

type assertionTrace struct {
	Channel string  `json:"channel"`
	Raw     float64 `json:"raw_score"`
	Fused   float64 `json:"fused_score"`
	Rank    int     `json:"rank"`
}
type assertionHit struct {
	priorScore            *scorePriorResult
	lexicalBase           float64
	OriginState           string              `json:"source_origin_state,omitempty"`
	PriorVersionID        string              `json:"prior_version_id,omitempty"`
	ID                    int64               `json:"assertion_id"`
	Version               int                 `json:"version"`
	Subject               string              `json:"subject"`
	Relation              string              `json:"relation"`
	Object                string              `json:"object"`
	Kind                  string              `json:"assertion_kind"`
	Lifecycle             string              `json:"lifecycle_state"`
	Authority             int                 `json:"authority_rank"`
	ConfidenceClass       string              `json:"confidence_class"`
	Confidence            float64             `json:"confidence"`
	ValidFrom             string              `json:"valid_from"`
	ValidUntil            string              `json:"valid_until"`
	AssertedAt            string              `json:"asserted_at"`
	SupersededAt          string              `json:"superseded_at"`
	Historical            bool                `json:"historical"`
	Support               int                 `json:"support_count"`
	Contradiction         int                 `json:"contradiction_count"`
	Evidence              []assertionEvidence `json:"evidence"`
	Retrieval             []assertionTrace    `json:"retrieval"`
	Hops                  int                 `json:"hop_depth"`
	Reason                string              `json:"inclusion_reason"`
	StableID              string              `json:"stable_id"`
	Rendered              string              `json:"rendered"`
	raw, fused            float64
	ownerID               string
	memoryParents         []MemoryRecordVersion
	memoryParentsObserved bool
}

func assertionTimestamp(value string) bool {
	if value == "" {
		return true
	}
	if len(value) != 19 && len(value) != 20 {
		return false
	}
	if len(value) == 20 && value[19] != 'Z' {
		return false
	}
	normalized := strings.ReplaceAll(strings.TrimSuffix(value, "Z"), "T", " ")
	stamp, err := time.Parse("2006-01-02 15:04:05", normalized)
	return err == nil && stamp.Year() > 0 && stamp.Format("2006-01-02 15:04:05") == normalized
}

// Public evidence routes share the same scoped data operations as host runtime
// calls. Preserve raw JSON fields so large assertion IDs never pass through floats.
func handleEvidenceCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	// Public RPC discovery does not grant plugins the host's data authority.
	if invocation.PrincipalRef != 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if verb == "assemble_typed_context" {
		return handleTypedContextResult(options, invocation, args, false)
	}
	raw, status := handleAssertionSearch(options, invocation, args)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	body, err := bus.DecodeCommandResult(raw)
	var result map[string]json.RawMessage
	if err != nil || json.Unmarshal(body, &result) != nil || result == nil {
		return nil, bus.ModuleStatusInternal
	}
	result["active_context_missing"], _ = json.Marshal(args.stringOr("project", "") == "" && args.stringOr("workspace", "") == "")
	return commandResult(result)
}

func handleAssertionSearch(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	query, ok := args.stringValue("query")
	if !ok || strings.TrimSpace(query) == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{Operation: "assertion-search", Query: query, Limit: args.limit("limit", 10, 64), Assertions: &assertionSearchRequest{}}
	for name, target := range map[string]*string{"valid_at": &request.Assertions.ValidAt, "believed_at": &request.Assertions.BelievedAt} {
		if raw, present := args[name]; present && (string(raw) == "null" || json.Unmarshal(raw, target) != nil) {
			return nil, bus.ModuleStatusInvalidRequest
		}
	}
	if raw, present := args["include_historical"]; present && (string(raw) == "null" || json.Unmarshal(raw, &request.Assertions.Historical) != nil) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if raw, present := args["max_hops"]; present && (string(raw) == "null" || json.Unmarshal(raw, &request.Assertions.Hops) != nil) {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if request.Assertions.Hops < 0 || request.Assertions.Hops > 2 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if !assertionTimestamp(request.Assertions.ValidAt) || !assertionTimestamp(request.Assertions.BelievedAt) {
		return commandResult(map[string]any{"status": "error", "error_type": "invalid_timestamp", "message": "timestamps must be second-precision UTC date-times", "assertions": []assertionHit{}})
	}
	// Explicit host context, never an implicit include-all request.
	request.Project, request.Workspace = args.stringOr("project", ""), args.stringOr("workspace", "")
	request.IncludeAll = args.boolean("include_all")
	if raw, present := args["scope"]; present && json.Unmarshal(raw, &request.Scope) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInvalidRequest || status == bus.ModuleStatusCancelled {
			return nil, status
		}
		return commandResult(map[string]any{"status": "degraded", "channel": "semantic_assertion", "reason": "semantic retrieval unavailable", "assertions": []assertionHit{}})
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(response.Payload)
}

// Every live memory evidence locator must resolve to a visible parent under
// the requested read policy. Explicit history permits retained old versions;
// it never bypasses erasure, revocation, quarantine, or scope checks.
// LEFT JOIN is intentional: RLS-hidden parents must deny the derived assertion,
// including assertions with a second, visible source. Apply exact scope before
// the candidate cap, even when the host has include-all authority.
func assertionParentPolicy() string {
	return `(CASE WHEN $1<>'' OR $2<>'' THEN (` +
		historicalMemoryInspectionSQL("m.") + ` AND ` + memoryValidityAtSQL("m.", "COALESCE("+memoryTimeSQL("$2::text")+",CURRENT_TIMESTAMP)") + `)
 WHEN $3 THEN (` + historicalMemoryInspectionSQL("m.") + `) ELSE (` + currentMemorySQL("m.") + `) END)`
}
func assertionVisible() string {
	return memoryEvidenceSQL("e", `$5='' OR (m.scope_type=$5 AND m.scope_value=$6)`, true, assertionParentPolicy())
}

// Belief time and world-valid time are independent half-open intervals. Do not
// truncate stored fractions or discard offsets when comparing either axis.
func assertionBeliefSQL(clock string) string {
	return memoryStartedAtSQL("e.asserted_at", clock) + ` AND ` +
		memoryUnexpiredAtSQL("e.superseded_at", clock) + ` AND ` +
		memoryUnexpiredAtSQL("e.invalidated_at", clock)
}

var assertionCurrent = `(` + assertionBeliefSQL("CURRENT_TIMESTAMP") + `
 AND (e.assertion_kind<>'world_fact' OR (` + memoryValiditySQL("e.") + `)))`

func assertionFilter() string {
	return `e.edge_class='semantic' AND e.suppressed=0 AND (e.lifecycle_state IN ('persistent','promoted') OR ($3 AND e.lifecycle_state='superseded'))
 AND ($1<>'' OR $3 OR (` + assertionBeliefSQL("CURRENT_TIMESTAMP") + `))
 AND ($1='' OR (` + assertionBeliefSQL(memoryTimeSQL("$1::text")) + `))
 AND ($2<>'' OR $3 OR e.assertion_kind<>'world_fact' OR (` + memoryValiditySQL("e.") + `))
 AND ($2='' OR (` + memoryValidityAtSQL("e.", memoryTimeSQL("$2::text")) + `)) AND ` + assertionVisible()
}
func assertionColumns() string {
	return `e.id,e.version,COALESCE((SELECT predecessor.id::text FROM entity_edges predecessor
 WHERE predecessor.id=e.prior_version_id AND ` + regexp.MustCompile(`\be\.`).ReplaceAllString(assertionFilter(), "predecessor.") + `),''),(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),e.source,e.relation,e.target,e.assertion_kind,e.lifecycle_state,
 e.authority_rank,e.confidence_class,e.confidence,e.valid_from,e.valid_until,e.asserted_at,e.superseded_at,
 NOT ` + assertionCurrent + `,
 (SELECT count(*) FROM fact_evidence f WHERE f.assertion_id=e.id AND f.invalidated_at='' AND f.stance='supports'),
 (SELECT count(*) FROM fact_evidence f WHERE f.assertion_id=e.id AND f.invalidated_at='' AND f.stance='contradicts')`
}

// Observe dependencies in the same MVCC snapshot as the assertion. The extra
// row detects overflow instead of silently certifying a prefix of the parents.
var assertionMemoryVersions = `COALESCE((SELECT jsonb_agg(jsonb_build_object('record_id',p.id::text,
 'record_revision',p.record_revision::text) ORDER BY p.id) FROM (
 SELECT DISTINCT m.id,m.record_revision FROM fact_evidence f CROSS JOIN LATERAL (
 SELECT m.id,m.record_revision FROM memories m WHERE m.id=` + memoryLocatorIDSQL("f.source_id") + ` LIMIT 1) m
 WHERE f.assertion_id=e.id AND f.source_kind='memory' AND f.invalidated_at=''
 ORDER BY m.id LIMIT ` + strconv.Itoa(maxTypedMemoryParents+1) + `) p),'[]'::jsonb)::text`

func assertionParams(request DataRequest, exact Scope, query string) []any {
	return []any{request.Assertions.BelievedAt, request.Assertions.ValidAt, request.Assertions.Historical, strings.ToLower(query), exact.Type, exact.Value}
}
func (s *postgresDataStore) assertionCandidates(ctx context.Context, request DataRequest, exact Scope, query string, limit int, vector string) ([]assertionHit, error) {
	params := assertionParams(request, exact, query)
	baseScore := `(CASE WHEN lower(e.source)=$4 OR lower(e.target)=$4 THEN 4.0 WHEN lower(e.relation)=$4 THEN 3.5 ELSE 1.0 END)`
	score := `(` + baseScore + `+LEAST(0.125,GREATEST(-0.125,
 LEAST(0.0625,GREATEST(-0.0625,e.confidence::double precision/16.0))+
 LEAST(0.0625,GREATEST(-0.0625,e.authority_rank::double precision/1600.0)))))`
	from := ` FROM entity_edges e WHERE ` + assertionFilter() + ` AND (lower(e.source) LIKE '%'||$4||'%' OR lower(e.relation) LIKE '%'||$4||'%' OR lower(e.target) LIKE '%'||$4||'%' OR lower(e.source||' '||e.relation||' '||e.target) LIKE '%'||$4||'%')`
	order := ` ORDER BY score DESC,e.authority_rank DESC,e.id DESC LIMIT $7`
	params = append(params, limit)
	if vector != "" {
		score = `1-(v.embedding <=> $8::vector)`
		from = ` FROM entity_edges e JOIN memory_embeddings v ON v.point_id=e.id+2000000000000 AND v.record_type='semantic_assertion' AND v.kind='assertion_v'||e.version::text WHERE ` + assertionFilter() + ` AND $4::text IS NOT NULL`
		order = ` ORDER BY v.embedding <=> $8::vector,e.id DESC LIMIT $7`
		params = append(params, vector)
	}
	if role := request.recoveryRole; role != nil && vector == "" {
		from = ` FROM entity_edges e WHERE ` + assertionFilter() + ` AND $4::text IS NOT NULL AND e.source=$8 AND e.relation=$9`
		params = append(params, role.Subject, role.Relation)
	}
	parentsSQL := `'[]'::text`
	if request.TypedContext != nil {
		parentsSQL = assertionMemoryVersions
	}
	rows, err := s.db.Query(ctx, `SELECT `+assertionColumns()+`,`+parentsSQL+`,`+baseScore+`,`+score+` AS score`+from+order, params...)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	hits := []assertionHit{}
	for rows.Next() {
		h := assertionHit{Evidence: []assertionEvidence{}, Retrieval: []assertionTrace{}}
		var parents string
		if err = rows.Scan(&h.ID, &h.Version, &h.PriorVersionID, &h.ownerID, &h.Subject, &h.Relation, &h.Object, &h.Kind, &h.Lifecycle, &h.Authority, &h.ConfidenceClass, &h.Confidence, &h.ValidFrom, &h.ValidUntil, &h.AssertedAt, &h.SupersededAt, &h.Historical, &h.Support, &h.Contradiction, &parents, &h.lexicalBase, &h.raw); err != nil {
			return nil, err
		}
		if err := json.Unmarshal([]byte(parents), &h.memoryParents); err != nil || len(h.memoryParents) > maxTypedMemoryParents {
			return nil, errors.New("memory: assertion dependency versions exceed the bounded projection capacity or are unavailable")
		}
		h.memoryParentsObserved = request.TypedContext != nil
		for i := range h.memoryParents {
			h.memoryParents[i].SchemaVersion = 1
			h.memoryParents[i].OwnerID = h.ownerID
			id, err := strconv.ParseInt(h.memoryParents[i].RecordID, 10, 64)
			if err != nil || !h.memoryParents[i].validFor(id) {
				return nil, errors.New("memory: assertion dependency version is unavailable")
			}
		}
		h.StableID = strconv.FormatInt(h.ID, 10)
		h.Rendered = h.Subject + " " + h.Relation + " " + h.Object
		if h.Historical {
			h.Rendered += " [HISTORICAL]"
		}
		h.Reason = "lexical semantic match after lifecycle, authority, and temporal filters"
		if vector == "" {
			proof := boundedScorePriors(assertionLexicalPriorPolicy, h.lexicalBase, assertionLexicalPriorBound, scorePriorAdjustment{Name: "confidence", Raw: h.Confidence / 16, Bound: .0625}, scorePriorAdjustment{Name: "authority", Raw: float64(h.Authority) / 1600, Bound: .0625})
			h.priorScore = &proof
			h.Retrieval = append(h.Retrieval, assertionTrace{Channel: "lexical", Raw: h.raw, Rank: len(hits) + 1})
		}
		hits = append(hits, h)
	}
	return hits, rows.Err()
}
func (s *postgresDataStore) assertionEvidence(ctx context.Context, h *assertionHit) error {
	rows, err := s.db.Query(ctx, `SELECT source_kind,source_id,source_span,observed_at,stance FROM fact_evidence WHERE assertion_id=$1 AND invalidated_at='' ORDER BY CASE WHEN stance='supports' THEN 0 ELSE 1 END,id DESC LIMIT 4`, h.ID)
	if err != nil {
		return err
	}
	defer rows.Close()
	for rows.Next() {
		var e assertionEvidence
		if err = rows.Scan(&e.SourceKind, &e.SourceID, &e.SourceSpan, &e.ObservedAt, &e.Stance); err != nil {
			return err
		}
		h.Evidence = append(h.Evidence, e)
	}
	return rows.Err()
}

func (s *postgresDataStore) assertionVectors(ctx context.Context, trace uint64, executor egress.Executor, request DataRequest, exact Scope) ([]assertionHit, int, error) {
	if executor == nil {
		return nil, 0, errors.New("embedding unavailable")
	}
	command, err := s.embeddingCommand("")
	if err != nil {
		return nil, 0, err
	}
	dim, err := s.vectorDimension(ctx)
	if err != nil {
		return nil, 0, err
	}
	modelVersion := ""
	if s.settings != nil {
		settings, settingsErr := s.settings()
		if settingsErr != nil {
			return nil, 0, settingsErr
		}
		modelVersion, _ = settings["embedder_model"].(string)
	}
	params := assertionParams(request, exact, "")
	rows, err := s.db.Query(ctx, `SELECT e.id,e.version,e.source||' '||e.relation||' '||e.target||' ['||e.assertion_kind||']' FROM entity_edges e LEFT JOIN memory_embeddings v ON v.point_id=e.id+2000000000000 AND v.record_type='semantic_assertion' WHERE `+assertionFilter()+` AND $4::text IS NOT NULL AND (v.point_id IS NULL OR v.kind<>'assertion_v'||e.version::text) ORDER BY e.id LIMIT 256`, params...)
	if err != nil {
		return nil, 0, err
	}
	type candidate struct {
		id      int64
		version int
		text    string
	}
	var candidates []candidate
	for rows.Next() {
		var c candidate
		if err = rows.Scan(&c.id, &c.version, &c.text); err != nil {
			rows.Close()
			return nil, 0, err
		}
		candidates = append(candidates, c)
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, 0, err
	}
	indexed := 0
	for _, c := range candidates {
		if c.id <= 0 || c.id > int64(^uint64(0)>>1)-assertionPointOffset {
			return nil, indexed, errors.New("assertion vector ID overflow")
		}
		if len(c.text) > maxDataBody {
			return nil, indexed, errors.New("assertion embedding input exceeds capacity")
		}
		screened, screenErr := screenMemoryText(c.text)
		if screenErr != nil {
			return nil, indexed, screenErr
		}
		embedded := Embed(ctx, trace, executor, EmbedRequest{BaseURL: command, InputType: "document", Text: screened, MaxDim: 4000})
		if embedded.Error != "" || embedded.Dim != dim || embedded.Truncated {
			return nil, indexed, errors.New("assertion embedding unavailable")
		}
		literal, err := vectorLiteral(embedded.Vector)
		if err != nil {
			return nil, indexed, err
		}
		payload, _ := json.Marshal(map[string]any{"record_type": "semantic_assertion", "assertion_id": c.id, "assertion_version": c.version, "canonical_rendering": c.text, "model_version": modelVersion})
		// Serialize publication with edits, then verify the exact version/content
		// selected before embedding. A late provider reply cannot revive stale data.
		tag, err := s.db.Exec(ctx, `WITH locked AS MATERIALIZED (SELECT id,version,source,relation,target,assertion_kind,lifecycle_state,suppressed FROM entity_edges WHERE id=$1 FOR UPDATE)
 INSERT INTO memory_embeddings(point_id,embedding,record_type,primary_scope,workspace,project,kind,payload_json)
 SELECT id+2000000000000,$2::vector,'semantic_assertion','','','','assertion_v'||version::text,$3 FROM locked
 WHERE version=$4 AND source||' '||relation||' '||target||' ['||assertion_kind||']'=$5 AND suppressed=0 AND lifecycle_state IN ('persistent','promoted')
 ON CONFLICT(point_id) DO UPDATE SET embedding=EXCLUDED.embedding,record_type=EXCLUDED.record_type,primary_scope='',workspace='',project='',kind=EXCLUDED.kind,payload_json=EXCLUDED.payload_json`, c.id, literal, string(payload), c.version, c.text)
		if err != nil {
			return nil, indexed, err
		}
		indexed += int(tag.RowsAffected())
	}
	screened, screenErr := screenMemoryText(request.Query)
	if screenErr != nil {
		return nil, indexed, screenErr
	}
	embedded := Embed(ctx, trace, executor, EmbedRequest{BaseURL: command, InputType: "query", Text: screened, MaxDim: 4000})
	if embedded.Error != "" || embedded.Dim != dim || embedded.Truncated {
		return nil, indexed, errors.New("assertion query embedding unavailable")
	}
	literal, err := vectorLiteral(embedded.Vector)
	if err != nil {
		return nil, indexed, err
	}
	hits, err := s.assertionCandidates(ctx, request, exact, request.Query, min(64, request.Limit*4), literal)
	return hits, indexed, err
}
func (s *postgresDataStore) searchAssertions(ctx context.Context, trace uint64, executor egress.Executor, request DataRequest, explicit bool) (map[string]any, error) {
	exact := Scope{}
	if explicit {
		exact = request.Scope
	}
	hits, err := s.assertionCandidates(ctx, request, exact, request.Query, request.Limit, "")
	if err != nil {
		return nil, err
	}
	recordRetrievalArm(ctx, "lexical", retrievalArmObservation{State: "available", Reason: "eligible_temporal_assertions", Candidates: len(hits), Quota: request.Limit, IndexReadiness: "query_executed"})
	// Optional vector errors roll back only derived indexing. They must not abort
	// the request transaction or turn successful lexical retrieval into empty data.
	if _, err = s.db.Exec(ctx, `SAVEPOINT assertion_vectors`); err != nil {
		return nil, err
	}
	vectors, indexed, vectorErr := s.assertionVectors(ctx, trace, executor, request, exact)
	if vectorErr != nil {
		indexed = 0
		if _, err = s.db.Exec(ctx, `ROLLBACK TO SAVEPOINT assertion_vectors`); err != nil {
			return nil, err
		}
	}
	if _, err = s.db.Exec(ctx, `RELEASE SAVEPOINT assertion_vectors`); err != nil {
		return nil, err
	}
	if vectorErr != nil {
		recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "unavailable", Reason: "assertion_embedding_or_index_fallback", Quota: min(64, request.Limit*4)})
	} else {
		recordRetrievalArm(ctx, "dense", retrievalArmObservation{State: "available", Reason: "version_filtered_assertion_vectors", Candidates: len(vectors), Quota: min(64, request.Limit*4), IndexReadiness: "eligible_versions_only; coverage_not_proven"})
	}
	find := func(id int64) int {
		for i := range hits {
			if hits[i].ID == id {
				return i
			}
		}
		return -1
	}
	overlap := 0
	if vectorErr == nil {
		for i, h := range vectors {
			if h.raw < 0.20 {
				continue
			}
			at := find(h.ID)
			if at < 0 {
				h.Reason = "vector semantic match after lifecycle, authority, scope, and temporal filters"
				hits = append(hits, h)
				at = len(hits) - 1
			} else {
				if hits[at].Version != h.Version || hits[at].ownerID != h.ownerID {
					return nil, errors.New("memory: assertion changed during candidate collection")
				}
				overlap++
			}
			hits[at].Retrieval = append(hits[at].Retrieval, assertionTrace{Channel: "vector", Raw: h.raw, Rank: i + 1})
		}
	}
	// Graph admission has its own candidate and lookup budgets. A full
	// lexical/dense union cannot consume this arm's slots before fusion.
	graphCount, expansions := 0, 0
	maxExpansions := min(64, 2*request.Limit)
	seenAnchors := map[string]bool{}
	start, end := 0, len(hits)
	for hop := 1; hop <= request.Assertions.Hops && start < end && expansions < maxExpansions; hop++ {
		for i := start; i < end && graphCount < request.Limit && expansions < maxExpansions; i++ {
			for _, anchor := range []string{hits[i].Subject, hits[i].Object} {
				if graphCount >= request.Limit {
					break
				}
				if seenAnchors[anchor] || expansions >= maxExpansions {
					continue
				}
				if err := ctx.Err(); err != nil {
					return nil, err
				}
				seenAnchors[anchor] = true
				expansions++
				expanded, err := s.assertionCandidates(ctx, request, exact, anchor, 16, "")
				if err != nil {
					return nil, err
				}
				for _, h := range expanded {
					if graphCount >= request.Limit {
						break
					}
					if find(h.ID) >= 0 || h.Subject != anchor && h.Object != anchor {
						continue
					}
					h.Hops = hop
					h.priorScore = nil
					h.Reason = fmt.Sprintf("bounded semantic hop %d with temporal and scope filters reapplied", hop)
					graphCount++
					// The anchor lookup is a graph vote, not lexical evidence
					// for the original user query. Its rank belongs to this arm.
					h.Retrieval = []assertionTrace{{Channel: "semantic_graph", Raw: 1 / float64(hop+1), Rank: graphCount}}
					hits = append(hits, h)
				}
			}
		}
		start, end = end, len(hits)
	}
	if request.Assertions.Hops > 0 {
		recordRetrievalArm(ctx, "graph", retrievalArmObservation{State: "available", Reason: "bounded_temporal_assertion_hops", Candidates: graphCount, Quota: request.Limit, IndexReadiness: "parent_and_temporal_filtered"})
	} else {
		recordRetrievalArm(ctx, "graph", retrievalArmObservation{State: "not_executed", Reason: "zero_requested_hops"})
	}
	lexicalOnly, vectorOnly := 0, 0
	for i := range hits {
		lexical, vector := false, false
		for _, r := range hits[i].Retrieval {
			hits[i].fused += assertionRRFContribution(r.Rank)
			lexical = lexical || r.Channel == "lexical"
			vector = vector || r.Channel == "vector"
		}
		if lexical && !vector {
			lexicalOnly++
		}
		if vector && !lexical {
			vectorOnly++
		}
		for j := range hits[i].Retrieval {
			hits[i].Retrieval[j].Fused = hits[i].fused
		}
	}
	sort.Slice(hits, func(i, j int) bool {
		if hits[i].fused != hits[j].fused {
			return hits[i].fused > hits[j].fused
		}
		if hits[i].Authority != hits[j].Authority {
			return hits[i].Authority > hits[j].Authority
		}
		return hits[i].ID > hits[j].ID
	})
	candidateCount := len(hits)
	if len(hits) > request.Limit {
		hits = hits[:request.Limit]
	}
	for i := range hits {
		if err = s.assertionEvidence(ctx, &hits[i]); err != nil {
			return nil, err
		}
	}
	result := map[string]any{"status": "ok", "channel": "semantic_assertion", "mode": "hybrid_shadow", "channel_status": "ok", "assertions": hits, "max_hops": request.Assertions.Hops, "indexed_assertions": indexed, "candidate_count": candidateCount, "candidate_scope": "independent_arm_union", "candidate_policy": "assertion-arm-union-v1", "graph_budget_exhausted": request.Assertions.Hops > 0 && (graphCount >= request.Limit || expansions >= maxExpansions), "graph_candidates": graphCount, "graph_expansions": expansions, "shadow_delta": map[string]int{"lexical_only": lexicalOnly, "vector_only": vectorOnly, "overlap": overlap}, "valid_at": request.Assertions.ValidAt, "believed_at": request.Assertions.BelievedAt, "include_historical": request.Assertions.Historical}
	if vectorErr != nil {
		result["mode"] = "lexical_degraded"
		result["channel_status"] = "degraded"
		result["degraded_reason"] = "embedding or vector index unavailable"
	}
	priorTraces := map[string]*scorePriorResult{}
	for _, hit := range hits {
		if hit.priorScore != nil {
			priorTraces[hit.StableID] = hit.priorScore
		}
	}
	result["score_prior_traces"] = priorTraces
	if capabilities := observedRetrievalCapabilities(ctx); capabilities != nil {
		result["retrieval_capabilities"] = capabilities
	}
	return result, nil
}
