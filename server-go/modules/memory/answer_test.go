package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"math"
	"reflect"
	"strings"
	"testing"

	"github.com/jackc/pgx/v5"
)

func TestAnswerGatePreservesNativeDecisions(t *testing.T) {
	rows := []publicMemoryRecord{{ID: 7, Tier: "L2", Kind: "fact", Key: "mars:color", Content: "mars color is red", RetrievalScore: 0.9, HybridRank: 1}}
	p := answerPolicy{threshold: 0.99}
	trace := answerGate(rows, 0, "mars color", 1, 0.95, p)
	if trace.Decision != "answerable" || trace.TopKGrounding != 0.8 || trace.AnchorCoverage != 1 || trace.ClusterCoverage != 1 || trace.AnchorRank != 1 {
		t.Fatal(trace)
	}
	p.abstain = true
	trace = answerGate(rows, 0, "mars color", 1, 0.95, p)
	if trace.Decision != "abstain" || trace.Reason != "grounding_low" {
		t.Fatal(trace)
	}
	rows[0].Tier = "L4"
	trace = answerGate(rows, 0, "mars color", 1, 0.95, p)
	if trace.Decision != "exempt" || trace.Reason != "curated_exempt" || !trace.Exempt {
		t.Fatal(trace)
	}
	rows[0].Tier = "L2"
	p.chunkFloor = 0.95
	if trace = answerGate(rows, 0, "mars color", 1, 0.95, p); trace.Reason != "chunk_floor" {
		t.Fatal(trace)
	}
	p.citations = "required"
	if trace = answerGate(rows, 0, "mars color", 0, 0.95, p); trace.Reason != "citation_required" || !trace.Structural {
		t.Fatal(trace)
	}
	if trace = answerGate(nil, 0, "mars color", 0, 0, p); trace.Reason != "structural_empty" || trace.AnchorRank != -1 || trace.CandidateIDs == nil {
		t.Fatal(trace)
	}
	p = answerPolicy{threshold: 0.81, abstain: true}
	if trace = answerGate(rows, 0, "mars color", 1, 0.85, p); trace.Decision != "answerable" {
		t.Fatal("threshold epsilon", trace)
	}
	if trace = answerGate(rows, 0, "mars color", 1, 0.75, p); trace.Decision != "abstain" {
		t.Fatal("epsilon must require answer support", trace)
	}
}

func TestAnswerIntentUsesWholeTerms(t *testing.T) {
	for query, want := range map[string]string{"update the candidate ranker": "general", "Chicago birthday sometimes": "general", "When did Jon visit?": "temporal", "who owns the service": "entity", "how does the owner deploy": "procedural", "next week deployment": "temporal"} {
		if got := answerIntent(query); got != want {
			t.Fatalf("%q: %s", query, got)
		}
	}
}

func TestDiagnosticRankingExcludesConfidence(t *testing.T) {
	typ := reflect.TypeOf(rankingInput{})
	if typ.NumField() != 2 || typ.Field(0).Name != "Key" || typ.Field(1).Name != "Content" {
		t.Fatal("ranking input widened", typ)
	}
	for _, confidence := range []float64{0.05, 0.8, 0.95} {
		p := diagnosticFor(Record{Key: "deployment", Content: "the project uses docker for deployment", Confidence: confidence}, "docker deployment").Parts
		sum := p.Lexical + p.Coverage + p.Entity + p.Temporal + p.Evidence + p.Semantic + p.State + p.Intent + p.Salience + p.Surprise + p.PageRank
		if math.Abs(p.Total-sum) > 1e-9 || p.Confidence != confidence || p.Total != 0.35 {
			t.Fatal(p)
		}
	}
}

func TestAnswerPublicValidation(t *testing.T) {
	client := clientForHandler(t, NewHandler(nil, WithDataStore(PlacementKB, nil)))
	for _, args := range []string{`{}`, `{"query":" "}`, `{"query":"mars","scope_type":"project"}`} {
		if r := runPublicCommand(t, client, "ask", args); r["kind"] != "invalid_argument" {
			t.Fatal(r)
		}
	}
}

func exerciseAnswerReplay(t *testing.T, ctx context.Context, tx pgx.Tx, backend *postgresDataStore) {
	t.Helper()
	_, err := tx.Exec(ctx, `SELECT set_config('aimee.memory_scope_all','1',true);
INSERT INTO memories(scope_type,scope_value,tier,kind,key,content,confidence,source_session,created_at,updated_at)
VALUES ('project','runtime-answer-a','L2','fact','mars:color','mars color is red',0.9,'answer-mars',pg_now_text(),pg_now_text()),
('project','runtime-answer-b','L2','fact','mars:color','mars color is blue',0.9,'answer-mars-b',pg_now_text(),pg_now_text()),
('project','runtime-answer-a','L4','fact','venus:color','venus color is yellow',0.9,'answer-venus',pg_now_text(),pg_now_text());`)
	if err != nil {
		t.Fatal(err)
	}
	var marsID int64
	if err := tx.QueryRow(ctx, `SELECT id FROM memories WHERE scope_value='runtime-answer-a' AND key='mars:color'`).Scan(&marsID); err != nil {
		t.Fatal(err)
	}
	enabled := false
	configured := *backend
	configured.settings = func() (map[string]any, error) {
		return map[string]any{"memory_abstain_enabled": enabled, "memory_abstain_gate": 0.99}, nil
	}
	handler := NewHandler(nil, WithDataStore(PlacementKB, &configured))
	client := clientForHandler(t, handler)
	run := func(query, extra string) map[string]any {
		t.Helper()
		args := fmt.Sprintf(`{"query":%q,"scope_context":true,"project":"runtime-answer-a"%s}`, query, extra)
		r := runPublicCommand(t, client, "ask", args)
		if r["status"] != "ok" {
			t.Fatal(r)
		}
		return r
	}
	r := run("mars color", "")
	if r["no_answer"] != false || r["answer"] != "mars color is red" || r["evidence_mode"] != "verbatim" {
		t.Fatal(r)
	}
	ids := r["citation_ids"].([]any)
	if len(ids) != 1 || ids[0] != float64(marsID) {
		t.Fatal(r)
	}
	enabled = true
	r = run("mars color", "")
	trace := r["evidence_trace"].(map[string]any)
	if r["no_answer"] != true || r["answer"] != "" || len(r["citation_ids"].([]any)) != 0 || trace["reason"] != "grounding_low" {
		t.Fatal(r)
	}
	r = run("venus color", "")
	trace = r["evidence_trace"].(map[string]any)
	if r["no_answer"] != false || trace["decision"] != "exempt" {
		t.Fatal(r)
	}
	r = run("nothingmatchesanswer", "")
	trace = r["evidence_trace"].(map[string]any)
	if r["no_answer"] != true || trace["reason"] != "structural_empty" || r["citation_ids"] == nil {
		t.Fatal(r)
	}
	enabled = false
	r = run("mars color", `,"scope_type":"project","scope_value":"runtime-answer-b"`)
	if r["answer"] != "mars color is blue" {
		t.Fatal("explicit scope ignored", r)
	}
	// Derived extraction is seeded by the schema owner, then read as the runtime role.
	if _, err := tx.Exec(ctx, `RESET ROLE`); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `INSERT INTO memory_event_frames(memory_id,actor,action,object,event_time) VALUES($1,'Alice','described','red','Tuesday')`, marsID); err != nil {
		t.Fatal(err)
	}
	if _, err := tx.Exec(ctx, `SET LOCAL ROLE aimee_store_runtime; SELECT set_config('aimee.memory_scope_all','1',true)`); err != nil {
		t.Fatal(err)
	}
	// Full-text query terms are held fixed; the event selection is exercised directly
	// against the same restricted store after retrieval IDs have been established.
	rows := []publicMemoryRecord{{ID: marsID, Content: "mars color is red"}}
	for query, want := range map[string]string{"when mars color": "Tuesday", "who mars color": "Alice", "mars color": "red"} {
		got, err := configured.extractAnswer(ctx, rows, 0, query)
		if err != nil || got != want {
			t.Fatal(query, got, err)
		}
	}
	// Confidence changes cannot reorder a tied retrieval or change its diagnostic score.
	_, err = tx.Exec(ctx, `INSERT INTO memories(scope_type,scope_value,tier,kind,key,content,confidence,created_at,updated_at)
VALUES('project','runtime-rank','L2','fact','rank-confidence-a','rank-confidence probe',0.1,'2026-01-01','2026-01-01'),
('project','runtime-rank','L2','fact','rank-confidence-b','rank-confidence probe',0.8,'2026-01-01','2026-01-01')`)
	if err != nil {
		t.Fatal(err)
	}
	search := func() string {
		r := runPublicCommand(t, client, "find_facts_scoped", `{"query":"rank-confidence probe","scope_type":"project","scope_value":"runtime-rank"}`)
		facts, _ := r["facts"].([]any)
		if len(facts) != 2 {
			t.Fatal(r)
		}
		ids := []any{}
		for _, f := range facts {
			ids = append(ids, f.(map[string]any)["id"])
		}
		raw, _ := json.Marshal(ids)
		return string(raw)
	}
	before := search()
	if _, err := tx.Exec(ctx, `UPDATE memories SET confidence=CASE WHEN key='rank-confidence-a' THEN 0.8 ELSE 0.1 END WHERE scope_value='runtime-rank'`); err != nil {
		t.Fatal(err)
	}
	if after := search(); after != before {
		t.Fatal("confidence reordered retrieval", before, after)
	}
}

func TestAnswerClusterCitationsAndMissingEvidence(t *testing.T) {
	rows := []publicMemoryRecord{{ID: 1, Kind: "fact", Content: "unrelated", SourceSession: "solo"}, {ID: 2, Kind: "episode", Content: "mars color", SourceSession: "mars"}, {ID: 3, Kind: "fact", Content: "mars red", SourceSession: "mars"}, {ID: 4, Kind: "fact", Content: "mars", SourceSession: "mars"}, {ID: 5, Kind: "fact", Content: "mars mission", SourceSession: "mars"}}
	anchor := answerAnchor(rows)
	if anchor != 1 {
		t.Fatal("cluster lost to isolated top row", anchor)
	}
	result := finishAnswer(rows, anchor, "mars color", "red", answerPolicy{threshold: 0.4})
	if !reflect.DeepEqual(result.CitationIDs, []int64{2, 3, 4, 5}) || result.Answer != "red" || result.Evidence.AnchorID != 2 {
		t.Fatal(result)
	}
	rows = []publicMemoryRecord{{Content: "unverified", Tier: "L2"}}
	for _, mode := range []string{"optional", "required"} {
		r := finishAnswer(rows, 0, "unverified", "unverified", answerPolicy{citations: mode, stripUnverified: true})
		if !r.NoAnswer || r.Answer != "" || r.Confidence != 0 || r.Evidence.Reason != "citation_required" {
			t.Fatal(r)
		}
	}
	r := finishAnswer(rows, 0, "unverified", "unverified", answerPolicy{citations: "off", stripUnverified: true})
	if r.NoAnswer || r.Answer == "" {
		t.Fatal("disabled citation policy stripped answer", r)
	}
	rows = []publicMemoryRecord{{ID: 1, Content: "restored", Tier: "L5"}}
	if r := finishAnswer(rows, 0, "restored", "restored", answerPolicy{}); r.EvidenceMode != "synthesised" || !r.Evidence.Exempt {
		t.Fatal(r)
	}
}

func TestAnswerCountersAndUnverifiedCuratedWarning(t *testing.T) {
	before := recallMetrics().AnswerCounters
	rows := []publicMemoryRecord{{Content: "curated", Tier: "L4"}}
	result := finishAnswer(rows, 0, "curated", "curated", answerPolicy{citations: "required"})
	if result.NoAnswer || !result.LowConfidence || !result.Evidence.Exempt || !strings.HasPrefix(result.Answer, "## Retrieval Confidence: LOW") {
		t.Fatal(result)
	}
	finishAnswer(rows, 0, "curated", "curated", answerPolicy{citations: "required", stripUnverified: true})
	rows[0].ID = 1
	finishAnswer(rows, 0, "curated", "curated", answerPolicy{citations: "required"})
	after := recallMetrics().AnswerCounters
	for key, delta := range map[string]int64{"memory.citation.missing": 2, "memory.citation.stripped": 1, "memory.citation.verified": 1} {
		if after[key]-before[key] != delta {
			t.Errorf("%s delta = %d", key, after[key]-before[key])
		}
	}
}
