package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"github.com/jackc/pgx/v5"
)

func TestAssertionTimestampsAndBoundary(t *testing.T) {
	for _, stamp := range []string{"", "2024-02-29T00:00:00Z", "2026-01-02 03:04:05", "2026-01-02T03:04:05"} {
		if !assertionTimestamp(stamp) {
			t.Fatal(stamp)
		}
	}
	for _, stamp := range []string{"0000-01-01T00:00:00Z", "2026-02-29T00:00:00Z", "2026-01-02T00:00:00.123Z", "2026-01-02T00:00:00+00:00", "2026-13-01T00:00:00Z", "2026-01-01T24:00:00Z"} {
		if assertionTimestamp(stamp) {
			t.Fatal(stamp)
		}
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, nil))
	for _, verb := range []string{"search_assertions", "assemble_typed_context"} {
		frame, _ := bus.EncodeCommand(verb, []byte(`{"query":"x","include_all":true}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("plugin gained host evidence authority", verb, status)
		}
	}
	for _, raw := range []string{`{"operation":"assertion-search","query":""}`, `{"operation":"assertion-search","query":"x","max_hops":3}`, `{"operation":"assertion-search","query":"x","include_historical":1}`, `{"operation":"assertion-search","query":"x","valid_at":null}`, `{"operation":"assertion-search","query":"x","include_historical":null}`} {
		public, _ := bus.EncodeCommand("search_assertions", []byte(raw))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, public); status != bus.ModuleStatusInvalidRequest {
			t.Fatal("public validation", raw, status)
		}
		frame, _ := bus.EncodeCommand("runtime", []byte(raw))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusInvalidRequest {
			t.Fatal(raw, status)
		}
	}
	frame, _ := bus.EncodeCommand("runtime", []byte(`{"operation":"assertion-search","query":"x"}`))
	if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 73}, frame); status != bus.ModuleStatusInvalidRequest {
		t.Fatal(status)
	}
	if _, status := NewHandler(nil, WithDataStore(PlacementServer, nil))(bus.ModuleInvocation{StageID: StageCommand}, frame); status != bus.ModuleStatusCapabilityAbsent {
		t.Fatal(status)
	}
	result := runHostRuntime(t, handler, `{"operation":"assertion-search","query":"x","valid_at":"2026-02-30T00:00:00Z"}`)
	if result["error_type"] != "invalid_timestamp" {
		t.Fatal(result)
	}
}

type assertionEgressFixture struct {
	dim   int
	seen  []string
	wrong bool
}

func (e *assertionEgressFixture) Do(_ context.Context, _ uint64, r egress.HTTPRequest) (egress.HTTPResponse, error) {
	e.seen = append(e.seen, string(r.Body))
	dim := e.dim
	if e.wrong {
		dim = 3
	}
	v := make([]float32, dim)
	v[0] = 1
	if strings.Contains(string(r.Body), "Casey") || strings.Contains(string(r.Body), "FairPool uses") {
		v[0] = 0
		v[1] = 1
	}
	raw, _ := json.Marshal(v)
	return egress.HTTPResponse{Status: 200, Body: raw}, nil
}
func exerciseAssertionSearchReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	exec := func(sql string, args ...any) {
		t.Helper()
		if _, err := tx.Exec(ctx, sql, args...); err != nil {
			t.Fatal(err)
		}
	}
	exec(`SAVEPOINT assertion_search_replay`)
	defer func() {
		exec(`ROLLBACK TO SAVEPOINT assertion_search_replay; RELEASE SAVEPOINT assertion_search_replay`)
	}()
	const old int64 = 9007199254742001
	const current = old + 1
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true);
 INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status) VALUES('assertion-search-fixture','assert','test','system',100,'open')`)
	exec(`INSERT INTO entity_edges(id,source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,valid_from,valid_until,asserted_at,superseded_at,commit_id) VALUES
 ($1,'AssertionAtlas','deployment_state','old','semantic','world_fact','persistent','A',.9,80,'2026-01-01T00:00:00Z','2026-03-01T00:00:00Z','2026-01-02T00:00:00Z','2026-03-02T00:00:00Z','assertion-search-fixture'),
 ($1+1,'AssertionAtlas','deployment_state','LinkNode','semantic','world_fact','persistent','A',.95,80,'2026-03-01T00:00:00Z','','2026-03-02T00:00:00Z','','assertion-search-fixture'),
 ($1+2,'LinkNode','owner','Casey','semantic','world_fact','persistent','A',.8,80,'','','','','assertion-search-fixture'),
 ($1+3,'VectorOnly','owner','Different','semantic','world_fact','persistent','A',.8,80,'','','','','assertion-search-fixture')`, old)
	exec(`INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,after_lifecycle,after_confidence,after_authority_rank,after_version) SELECT 'assertion-search-fixture',id,'assert',0,1,'persistent',confidence,80,1 FROM entity_edges WHERE commit_id='assertion-search-fixture'`)
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,source_span,evidence_hash,observed_at,stance) VALUES($1,'episode','event:41','bytes:4-19','assertion-search-fixture','2026-01-02T00:00:00Z','supports')`, old)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	handler := NewHandler(nil, WithDataStore(PlacementKB, backend))
	args := map[string]any{"operation": "assertion-search", "query": "AssertionAtlas", "valid_at": "2026-02-01T00:00:00Z", "believed_at": "2026-02-02T00:00:00Z"}
	call := func() map[string]any {
		raw, _ := json.Marshal(args)
		result := runPublicCommand(t, clientForHandler(t, func(invocation bus.ModuleInvocation, frame []byte) ([]byte, bus.ModuleStatus) {
			// The KB RPC host invokes these fixed-owner data operations.
			invocation.PrincipalRef = 0
			return handler(invocation, frame)
		}), "search_assertions", string(raw))
		if result["active_context_missing"] != (args["project"] == nil && args["workspace"] == nil) {
			t.Fatal("missing-context metadata", result)
		}
		return result
	}
	hits := func(result map[string]any) []any { t.Helper(); return result["assertions"].([]any) }
	got := call()
	if got["mode"] != "lexical_degraded" || len(hits(got)) != 1 {
		t.Fatal(got)
	}
	first := hits(got)[0].(map[string]any)
	if first["stable_id"] != fmt.Sprint(old) || first["historical"] != true || first["evidence"].([]any)[0].(map[string]any)["source_span"] != "bytes:4-19" {
		t.Fatal(first)
	}
	args["valid_at"], args["believed_at"] = "2026-04-01T00:00:00Z", "2026-04-02T00:00:00Z"
	got = call()
	if len(hits(got)) != 1 || hits(got)[0].(map[string]any)["stable_id"] != fmt.Sprint(current) {
		t.Fatal(got)
	}
	envelope := runHostRuntime(t, handler, `{"operation":"typed-context","query":"AssertionAtlas","enable_observations":false,"enable_approved_procedures":false}`)
	var projection typedContextResult
	if err := json.Unmarshal([]byte(envelope["json"].(string)), &projection); err != nil {
		t.Fatal(err)
	}
	if len(projection.Retained) != 1 || projection.SourceVersionState != "record_versions_observed" || projection.Retained[0].Source == nil {
		t.Fatal("typed assertion source observation missing", projection)
	}
	source := projection.Retained[0].Source
	var owner string
	if err := tx.QueryRow(ctx, `SELECT owner_id::text FROM memory_collection_owner WHERE id=1`).Scan(&owner); err != nil {
		t.Fatal(err)
	}
	if source.Kind != "semantic_assertion" || source.Version.OwnerID != owner || source.Version.RecordID != fmt.Sprint(current) || source.Version.RecordRevision != "1" {
		t.Fatal("source version disagrees with the selected owner row", projection)
	}

	t.Run("bounded selection behind thirty duplicate SQL candidates", func(t *testing.T) {
		t.Setenv("AIMEE_MEMORY_SELECTION_POLICY", typedSelectionPolicyVersion)
		exec(`SAVEPOINT mr09_selector; RESET ROLE`)
		defer exec(`ROLLBACK TO SAVEPOINT mr09_selector; RELEASE SAVEPOINT mr09_selector`)
		exec(`INSERT INTO entity_edges(id,source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,commit_id)
 SELECT 9007199254790000+i,CASE WHEN i=0 THEN 'required-service' ELSE 'duplicate-service' END,'uses','MR09Selector','semantic','world_fact','persistent','A',.8,80,'assertion-search-fixture' FROM generate_series(0,30) i`)
		exec(`INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,after_lifecycle,after_confidence,after_authority_rank,after_version)
 SELECT 'assertion-search-fixture',id,'assert',0,1,lifecycle_state,confidence,authority_rank,version FROM entity_edges WHERE id BETWEEN 9007199254790000 AND 9007199254790030`)
		exec(`SET LOCAL ROLE aimee_store_runtime`)
		envelope := runHostRuntime(t, handler, `{"operation":"typed-context","query":"MR09Selector","enable_observations":false,"enable_approved_procedures":false,"context_limits":{"schema_version":1,"max_context_bytes":1600},"evidence_requirements":{"schema_version":1,"task_revision":"mr09-pg:1","query_mode":"current_state","obligations":[{"subject":"required-service","relation":"uses"}]}}`)
		var result typedContextResult
		if json.Unmarshal([]byte(envelope["json"].(string)), &result) != nil || result.SelectionPolicy == nil || result.Coverage == nil || result.Coverage.Roles[0].Status != "unavailable" || result.RenderedBytes > 1600 || len(result.Retained) == 0 || result.Retained[0].ID != "9007199254790000" {
			t.Fatal("actual owner selector lost required hit or concealed dense unavailability", result.Retained, result.Coverage, result.RenderedBytes)
		}
		if len(result.ScorePriorTraces) != 31 {
			t.Fatal("SQL prior evidence missing", len(result.ScorePriorTraces))
		}

		for _, item := range result.Channels["current_assertions"].Items {
			raw, _ := json.Marshal(item)
			var h assertionHit
			if json.Unmarshal(raw, &h) != nil || len(h.Retrieval) != 1 || h.Retrieval[0].Raw != result.ScorePriorTraces[h.StableID].Final {
				t.Fatal("SQL score differs from native contribution proof", h.StableID)
			}
		}
		for _, p := range result.ScorePriorTraces {
			if !validScorePriorResult(p) || p.Base != 4 {
				t.Fatal("invalid native SQL prior", p)
			}
		}
	})
	// Stored offsets and subsecond endpoints must compare as instants, even
	// though this public request contract retains second-precision UTC anchors.
	exec(`SAVEPOINT assertion_instant; SET LOCAL TIME ZONE 'Asia/Tokyo'`)
	exec(`UPDATE entity_edges SET valid_from='2026-04-01T09:00:00+09:00',
 valid_until='2026-04-01T00:00:00.500Z',asserted_at='2026-04-02T09:00:00+09:00',
 superseded_at='2026-04-02T00:00:00.500Z' WHERE id=$1`, current)
	got = call()
	if len(hits(got)) != 1 || hits(got)[0].(map[string]any)["stable_id"] != fmt.Sprint(current) {
		t.Fatal("offset/fraction interval lost", got)
	}
	// The same instant rendered as a positive offset is an exclusive endpoint.
	for _, column := range []string{"valid_until", "superseded_at", "invalidated_at"} {
		exec(`SAVEPOINT assertion_upper_boundary`)
		end := "2026-04-02T09:00:00+09:00"
		if column == "valid_until" {
			end = "2026-04-01T09:00:00+09:00"
		}
		exec(`UPDATE entity_edges SET `+column+`=$1 WHERE id=$2`, end, current)
		if got := call(); len(hits(got)) != 0 {
			t.Fatal("exclusive endpoint admitted", column, got)
		}
		exec(`ROLLBACK TO SAVEPOINT assertion_upper_boundary; RELEASE SAVEPOINT assertion_upper_boundary`)
	}
	exec(`UPDATE entity_edges SET valid_from='2026-04-01T00:00:00.001Z' WHERE id=$1`, current)
	if got := call(); len(hits(got)) != 0 {
		t.Fatal("future fraction admitted", got)
	}
	for _, bad := range []string{"now", "infinity", "2026-02-30T00:00:00Z"} {
		exec(`SAVEPOINT assertion_malformed_time`)
		exec(`UPDATE entity_edges SET valid_from=$1 WHERE id=$2`, bad, current)
		got = call()
		if got["status"] != "degraded" || len(hits(got)) != 0 {
			t.Fatal("malformed assertion time admitted", bad, got)
		}
		exec(`ROLLBACK TO SAVEPOINT assertion_malformed_time; RELEASE SAVEPOINT assertion_malformed_time`)
	}
	exec(`ROLLBACK TO SAVEPOINT assertion_instant; RELEASE SAVEPOINT assertion_instant`)
	args["believed_at"] = "2026-02-02T00:00:00Z"
	if len(hits(call())) != 0 {
		t.Fatal("time axes collapsed")
	}
	delete(args, "valid_at")
	delete(args, "believed_at")
	args["include_historical"] = true
	if len(hits(call())) != 2 {
		t.Fatal("explicit history lost")
	}
	delete(args, "include_historical")
	args["max_hops"] = 1
	got = call()
	if len(hits(got)) != 2 {
		t.Fatal(got)
	}
	foundHop := false
	for _, raw := range hits(got) {
		h := raw.(map[string]any)
		foundHop = foundHop || h["hop_depth"] == float64(1)
	}
	if !foundHop {
		t.Fatal("graph hop absent", got)
	}
	delete(args, "max_hops")
	// Canonical numeric tokens and the exact decimal ID survive serialization.
	raw, _ := json.Marshal(args)
	frame, _ := bus.EncodeCommand("search_assertions", raw)
	encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
	if status != bus.ModuleStatusOK || !strings.Contains(string(encoded), `"assertion_id":9007199254742002`) {
		t.Fatal(string(encoded), status)
	}
	// Scope filtering is deny-dominant across mixed visible/hidden evidence.
	exec(`RESET ROLE; SELECT set_config('aimee.memory_scope_all','1',true)`)
	var local, private int64
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','assertion-local','safe','project','assertion-local') RETURNING id`).Scan(&local); err != nil {
		t.Fatal(err)
	}
	if err := tx.QueryRow(ctx, `INSERT INTO memories(tier,kind,key,content,scope_type,scope_value) VALUES('L2','fact','assertion-private','private','project','assertion-private') RETURNING id`).Scan(&private); err != nil {
		t.Fatal(err)
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,evidence_hash,stance) VALUES($1,'memory','memory:'||$2::bigint::text,'local','supports')`, current, local)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	if len(hits(call())) != 0 {
		t.Fatal("missing context exposed memory assertion")
	}
	args["project"] = "assertion-local"
	if len(hits(call())) != 1 {
		t.Fatal("visible assertion hidden")
	}
	parentProjection := func() typedContextResult {
		t.Helper()
		envelope := runHostRuntime(t, handler, `{"operation":"typed-context","query":"AssertionAtlas","project":"assertion-local","enable_observations":false,"enable_approved_procedures":false}`)
		var result typedContextResult
		if err := json.Unmarshal([]byte(envelope["json"].(string)), &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	beforeParent := parentProjection()
	if len(beforeParent.Retained) != 1 || beforeParent.Retained[0].Source == nil || len(beforeParent.Retained[0].Source.MemoryParents) != 1 || beforeParent.Retained[0].Source.MemoryParentState != "observed" {
		t.Fatal("typed source omitted memory dependencies", beforeParent)
	}
	parentVersion := beforeParent.Retained[0].Source.MemoryParents[0]
	if parentVersion.OwnerID != owner || parentVersion.RecordID != fmt.Sprint(local) || parentVersion.RecordRevision != "1" {
		t.Fatal("parent version did not match the selected snapshot", parentVersion)
	}
	exec(`UPDATE memories SET content='changed supporting content' WHERE id=$1`, local)
	afterParent := parentProjection()
	if len(afterParent.Retained) != 1 || afterParent.Retained[0].Source.MemoryParents[0].RecordRevision != "2" || beforeParent.Rendered != afterParent.Rendered || beforeParent.SelectionDigest == afterParent.SelectionDigest {
		t.Fatal("changed parent reused a binding for identical assertion bytes", beforeParent, afterParent)
	}
	exec(`SAVEPOINT typed_parent_capacity`)
	exec(`WITH parents AS (INSERT INTO memories(tier,kind,key,content,scope_type,scope_value)
 SELECT 'L2','fact','typed-parent-'||n,'source','project','assertion-local' FROM generate_series(1,$1) n RETURNING id)
 INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance)
 SELECT $2,'memory','memory:'||id::text,'supports' FROM parents`, maxTypedMemoryParents, current)
	bounded := parentProjection()
	if bounded.Availability != "degraded" || len(bounded.Retained) != 0 || bounded.SourceVersionState != "unavailable" {
		t.Fatal("dependency overflow certified a prefix", bounded)
	}
	if len(hits(call())) != 1 {
		t.Fatal("typed metadata capacity changed ordinary assertion search")
	}
	exec(`ROLLBACK TO SAVEPOINT typed_parent_capacity; RELEASE SAVEPOINT typed_parent_capacity`)
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,evidence_hash,stance) VALUES($1,'memory','memory:'||$2::bigint::text,'private','supports')`, current, private)
	if len(hits(call())) != 0 {
		t.Fatal("visible source masked hidden evidence")
	}
	args["include_all"] = true
	if len(hits(call())) != 1 {
		t.Fatal("host include-all ignored")
	}
	args["scope"] = Scope{Type: ScopeProject, Value: "assertion-local"}
	if len(hits(call())) != 0 {
		t.Fatal("exact scope widened by include-all")
	}
	delete(args, "scope")
	delete(args, "include_all")
	exec(`RESET ROLE`)
	exec(`DELETE FROM fact_evidence WHERE assertion_id=$1 AND evidence_hash='private'`, current)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	bound := *backend
	bound.settings = func() (map[string]any, error) { return map[string]any{"embedder_url": "http://assertion-fixture"}, nil }
	dim, err := bound.vectorDimension(ctx)
	if err != nil {
		t.Fatal(err)
	}
	executor := &assertionEgressFixture{dim: dim}
	handler = NewHandler(executor, WithDataStore(PlacementKB, &bound))
	got = call()
	if got["mode"] != "hybrid_shadow" || got["indexed_assertions"].(float64) < 2 {
		t.Fatal(got)
	}
	vectorFound := false
	for _, raw := range hits(got) {
		h := raw.(map[string]any)
		vectorFound = vectorFound || h["subject"] == "VectorOnly"
	}
	if !vectorFound {
		t.Fatal("vector-only candidate missing", got)
	}
	// The public typed endpoint evaluates obligations over the actual scoped
	// PostgreSQL selection; vector retrieval is healthy for this fixture.
	coverageArgs := map[string]any{"operation": "typed-context", "query": "AssertionAtlas", "project": "assertion-local",
		"enable_observations": false, "enable_approved_procedures": false, "channel_budgets": map[string]int{"total": 4096, "current_assertions": 4096},
		"evidence_requirements": evidenceRequirementSet{SchemaVersion: 1, TaskRevision: "replay:1", QueryMode: "current_state", Obligations: []evidenceObligation{{Subject: "AssertionAtlas", Relation: "deployment_state"}}}}
	coverageCall := func() typedContextResult {
		t.Helper()
		raw, _ := json.Marshal(coverageArgs)
		out := runHostRuntime(t, handler, string(raw))
		var result typedContextResult
		if err := json.Unmarshal([]byte(out["json"].(string)), &result); err != nil {
			t.Fatal(err)
		}
		return result
	}
	covered := coverageCall()
	if covered.Sufficiency != "complete" || covered.Coverage == nil || len(covered.Coverage.Roles[0].Retained) != 1 || covered.Coverage.Roles[0].Retained[0] != fmt.Sprint(current) {
		t.Fatal("actual scoped role not covered", covered.Coverage, covered.Reason)
	}
	coverageArgs["context_limits"] = map[string]any{"schema_version": 1, "max_context_bytes": 0}
	dropped := coverageCall()
	if dropped.Sufficiency != "insufficient" || dropped.Coverage.Roles[0].Status != "budget_dropped" {
		t.Fatal("packed-away evidence still complete", dropped.Coverage)
	}
	delete(coverageArgs, "context_limits")
	exec(`SAVEPOINT coverage_hidden; RESET ROLE`)
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance,evidence_hash) VALUES($1,'memory',$2,'supports','coverage-hidden')`, current, "memory:"+fmt.Sprint(private))
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	hiddenCoverage := coverageCall()
	if hiddenCoverage.Sufficiency == "complete" || len(hiddenCoverage.Coverage.Roles[0].Retained) != 0 {
		t.Fatal("hidden parent establishes coverage", hiddenCoverage.Coverage)
	}
	exec(`ROLLBACK TO SAVEPOINT coverage_hidden; RELEASE SAVEPOINT coverage_hidden`)
	// A full lexical pool must not veto independently collected dense or
	// graph-only candidates before the final top-k decision.
	exec(`SAVEPOINT assertion_fair_pools; RESET ROLE`)
	exec(`INSERT INTO fact_graph_commits(commit_id,operation,actor_principal,actor_role,authority_rank,status)
 VALUES('assertion-fair-pools','assert','test','system',100,'open')`)
	exec(`INSERT INTO entity_edges(id,source,relation,target,edge_class,assertion_kind,lifecycle_state,confidence_class,confidence,authority_rank,commit_id) VALUES
 ($1,'FairPool','uses','alpha','semantic','world_fact','persistent','A',.8,80,'assertion-fair-pools'),
 ($1+1,'FairPool','uses','FairBridge','semantic','world_fact','persistent','A',.8,80,'assertion-fair-pools'),
 ($1+2,'DenseOnlyPrize','uses','omega','semantic','world_fact','persistent','A',.9,90,'assertion-fair-pools'),
 ($1+3,'FairBridge','uses','CaseyGraphPrize','semantic','world_fact','persistent','A',1,100,'assertion-fair-pools')`, old+100)
	exec(`INSERT INTO fact_graph_changes(commit_id,assertion_id,action,existed_before,existed_after,after_lifecycle,after_confidence,after_authority_rank,after_version)
 SELECT 'assertion-fair-pools',id,'assert',0,1,lifecycle_state,confidence,authority_rank,version FROM entity_edges WHERE commit_id='assertion-fair-pools'`)
	exec(`SET LOCAL ROLE aimee_store_runtime`)
	savedArgs := args
	args = map[string]any{"operation": "assertion-search", "query": "FairPool", "project": "assertion-local", "limit": 2}
	fair := call()
	containsID := func(result map[string]any, id int64) bool {
		for _, raw := range hits(result) {
			if raw.(map[string]any)["stable_id"] == fmt.Sprint(id) {
				return true
			}
		}
		return false
	}
	if len(hits(fair)) != 2 || !containsID(fair, old+102) {
		t.Fatal("full lexical pool excluded dense-only evidence", fair)
	}
	if fair["candidate_count"].(float64) <= 2 || fair["candidate_count"].(float64) > 10 {
		t.Fatal("independent arm union was not retained until fusion", fair)
	}
	args["max_hops"] = 1
	graphFair := call()
	if len(hits(graphFair)) != 2 || !containsID(graphFair, old+103) || graphFair["graph_candidates"] != float64(1) {
		t.Fatal("full base pool excluded graph-only evidence", graphFair)
	}
	for _, raw := range hits(graphFair) {
		h := raw.(map[string]any)
		if h["stable_id"] == fmt.Sprint(old+103) {
			trace := h["retrieval"].([]any)
			if len(trace) != 1 || trace[0].(map[string]any)["channel"] != "semantic_graph" || trace[0].(map[string]any)["rank"] != float64(1) {
				t.Fatal("graph anchor query invented an original lexical vote", trace)
			}
		}
	}
	exec(`INSERT INTO fact_evidence(assertion_id,source_kind,source_id,stance,evidence_hash) VALUES($1,'memory',$2,'supports','fair-hidden')`, old+103, "memory:"+fmt.Sprint(private))
	if hidden := call(); containsID(hidden, old+103) {
		t.Fatal("fair graph arm bypassed hidden parent", hidden)
	}
	args = savedArgs
	exec(`ROLLBACK TO SAVEPOINT assertion_fair_pools; RELEASE SAVEPOINT assertion_fair_pools`)
	before := len(executor.seen)
	got = call()
	if got["indexed_assertions"] != float64(0) || len(executor.seen) != before+1 {
		t.Fatal("current vectors reindexed", got, len(executor.seen)-before)
	}
	executor.wrong = true
	got = call()
	if got["mode"] != "lexical_degraded" || len(hits(got)) != 1 {
		t.Fatal("dimension mismatch erased lexical recall", got)
	}
	executor.wrong = false
	// Optional SQL failure rolls back to the savepoint and retains lexical evidence.
	exec(`SAVEPOINT assertion_vector_denied; RESET ROLE; REVOKE SELECT ON memory_embeddings FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	got = call()
	if got["mode"] != "lexical_degraded" || len(hits(got)) != 1 || got["indexed_assertions"] != float64(0) {
		t.Fatal("vector SQL failure lost lexical evidence", got)
	}
	exec(`ROLLBACK TO SAVEPOINT assertion_vector_denied; RELEASE SAVEPOINT assertion_vector_denied`)
	// Query credentials are screened before outbound embedding, even when the
	// query has no lexical match and vector candidates remain available.
	args["query"] = "token=assertion-fixture-secret"
	before = len(executor.seen)
	call()
	if len(executor.seen) <= before {
		t.Fatal("query embedding not exercised")
	}
	for _, sent := range executor.seen[before:] {
		if strings.Contains(sent, "assertion-fixture-secret") {
			t.Fatal("credential reached embedder")
		}
	}
	args["query"] = "AssertionAtlas"
	// Required SQL failures cannot masquerade as successful empty assertions.
	exec(`SAVEPOINT assertion_denied; RESET ROLE; REVOKE SELECT ON fact_evidence FROM aimee_store_runtime; SET LOCAL ROLE aimee_store_runtime`)
	raw, _ = json.Marshal(args)
	frame, _ = bus.EncodeCommand("search_assertions", raw)
	failed, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
	if status != bus.ModuleStatusOK || !strings.Contains(string(failed), `"status":"degraded"`) {
		t.Fatal(status, string(failed))
	}
	if got := call(); got["status"] != "degraded" {
		t.Fatal("public client lost degraded receipt", got)
	}
	exec(`ROLLBACK TO SAVEPOINT assertion_denied; RELEASE SAVEPOINT assertion_denied`)
}
