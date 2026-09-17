package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/db"
)

type publicDiagnostic struct {
	Memory publicMemoryRecord `json:"memory"`
	Parts  DiagnosticParts    `json:"parts"`
}

func (args commandArgs) boolean(name string) bool {
	var value bool
	return json.Unmarshal(args[name], &value) == nil && value
}

func handleDiagnosticCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, ok := args.stringValue("query")
	if !ok {
		return commandResult(commandError("invalid_argument", "missing query"))
	}
	request := DataRequest{Query: query, IncludeAll: true, PublicView: true, Limit: args.limit("limit", 10, 64)}
	scoped := false
	if verb == "explain_match" {
		request.Operation = "explain"
		request.ID, ok = args.positiveID("memory_id")
		if !ok {
			return commandResult(commandError("invalid_argument", "missing positive memory_id"))
		}
		scoped = commandScope(args, &request)
	} else {
		request.Operation = "diagnose"
		request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
		if request.Scope.Type == "" && request.Scope.Value == "" {
			scoped = commandScope(args, &request)
		} else {
			request.IncludeAll = false
			if _, err := normalizeScope(PlacementKB, request.Scope); err != nil {
				return commandResult(commandError("invalid_argument", err.Error()))
			}
		}
	}
	encoded, _ := json.Marshal(request)
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory retrieval unavailable or id missing"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	metadata := make(map[int64]publicMemoryRecord, len(response.PublicRecords))
	for _, r := range response.PublicRecords {
		metadata[r.ID] = r
	}
	rows := make([]publicDiagnostic, 0, len(response.Diagnostics))
	for _, d := range response.Diagnostics {
		if record, ok := metadata[d.Memory.ID]; ok {
			rows = append(rows, publicDiagnostic{Memory: record, Parts: d.Parts})
		}
	}
	result := map[string]any{"status": "ok"}
	if verb == "explain_match" {
		if len(rows) != 1 {
			return commandResult(commandError("not_found", "memory not found"))
		}
		result["row"] = rows[0]
	} else {
		result["rows"] = rows
		if args.boolean("trace") {
			attachDiagnosticTrace(options, invocation, args, request.Scope, response.Diagnostics, rows, result)
		}
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

// This is the existing fingerprint contract, whose offset differs from the
// standard FNV-1a offset. Keep it stable for previously persisted events.
func traceFingerprint(query string) string {
	h := uint64(1469598103934665603)
	for _, b := range []byte(query) {
		h = (h ^ uint64(b)) * 1099511628211
	}
	return fmt.Sprintf("%016x", h)
}

func diagnosticTraceRows(diagnostics []Diagnostic, rows []publicDiagnostic) []map[string]any {
	epistemic := map[int64]string{}
	for _, d := range diagnostics {
		epistemic[d.Memory.ID] = d.EpistemicKind
	}
	results := make([]map[string]any, 0, len(rows))
	for i, row := range rows {
		p, m := row.Parts, row.Memory
		final := p.Total
		if m.HybridRank > 0 {
			final = m.RetrievalScore
		}
		values := map[string]float64{"lexical": p.Lexical, "coverage": p.Coverage,
			"entity": p.Entity, "temporal": p.Temporal, "evidence": p.Evidence,
			"semantic": p.Semantic, "state": p.State, "intent": p.Intent,
			"salience": p.Salience, "surprise": p.Surprise, "pagerank": p.PageRank,
			"graph": p.GraphScore, "outcome": p.Outcome}
		weights, contributions := map[string]float64{}, map[string]float64{}
		explained := 0.0
		// Fixed order makes residual calculation deterministic.
		for _, feature := range []string{"lexical", "coverage", "entity", "temporal", "evidence", "semantic", "state", "intent", "salience", "surprise", "pagerank", "graph", "outcome"} {
			weight := 1.0
			if feature == "graph" {
				weight = p.GraphWeight
			}
			weights[feature] = weight
			contributions[feature] = values[feature] * weight
			explained += contributions[feature]
		}
		values["post_rank_residual"], weights["post_rank_residual"], contributions["post_rank_residual"] = final-explained, 1, final-explained
		kind := epistemic[m.ID]
		if kind == "" {
			kind = "world_fact"
		}
		results = append(results, map[string]any{
			"subject_kind": "memory", "subject_id": strconv.FormatInt(m.ID, 10), "lane": "hybrid",
			"lane_rank": i + 1, "final_rank": i + 1, "scope_decision": "allowed",
			"semantic_value": p.Semantic, "semantic_weight": 1, "keyword_value": p.Lexical + p.Coverage, "keyword_weight": 1,
			"graph_value": p.GraphScore, "graph_weight": p.GraphWeight, "temporal_value": p.Temporal, "temporal_weight": 1,
			"outcome_value": p.Outcome, "outcome_weight": 1, "final_score": final,
			"feature_values": values, "feature_weights": weights, "feature_contributions": contributions,
			"authority_class": m.ProvenanceCategory, "confidence_class": "not-computed", "epistemic_kind": kind,
			"valid_time_match": "not-computed", "source_evidence": m.SourceSession,
			"document_state": "not-computed", "staleness_status": "not-computed",
		})
	}
	return results
}

func attachDiagnosticTrace(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, scope Scope, diagnostics []Diagnostic, rows []publicDiagnostic, result map[string]any) {
	caller := options.commandContext
	if caller == nil || !caller.Authenticated {
		result["trace_status"] = "authenticated actor required"
		return
	}
	if scope.Type == "" || scope.Value == "" {
		scope = Scope{Type: caller.ScopeKind, Value: caller.ScopeID}
		if scope.Type == "" {
			scope = Scope{Type: "global"}
		}
	}
	fingerprint := traceFingerprint(args.stringOr("query", ""))
	now := time.Now()
	event := fmt.Sprintf("diagnose:%s:%d:%d", fingerprint, now.Unix(), now.Nanosecond())
	traceRows, _ := json.Marshal(diagnosticTraceRows(diagnostics, rows))
	backend, ok := options.data.(*postgresDataStore)
	if !ok {
		result["trace_status"] = "trace recording failed"
		return
	}
	db, ok := backend.db.(store.DB)
	if !ok {
		result["trace_status"] = "trace recording failed"
		return
	}
	ctx, cancel := context.WithTimeout(context.Background(), invocation.Remaining(30*time.Second))
	defer cancel()
	// The read has already succeeded. Trace failure rolls back only this
	// transaction and never turns an otherwise valid diagnostic into an error.
	tx, err := db.Begin(ctx)
	if err != nil {
		result["trace_status"] = "trace recording failed"
		return
	}
	defer tx.Rollback(context.Background())
	transport := caller.TransportIdentity
	if transport == "" {
		transport = caller.Principal
	}
	_, err = tx.Exec(ctx, `SELECT set_config('aimee.principal',$1,true),set_config('aimee.authority','user',true),set_config('aimee.transport_identity',$2,true)`, caller.Principal, transport)
	var raw string
	if err == nil {
		err = tx.QueryRow(ctx, `SELECT recall_trace_record($1,$2,$3,$4,$5,$6,$7::jsonb,$8)::text`,
			args.stringOr("retrieval_event_id", event), args.stringOr("turn_id", event), fingerprint, scope.Type, scope.Value,
			args.stringOr("sensitivity", "normal"), string(traceRows), args.boolean("persist_trace")).Scan(&raw)
	}
	if err == nil {
		err = tx.Commit(ctx)
	}
	if err != nil || !json.Valid([]byte(raw)) {
		result["trace_status"] = "trace recording failed"
		return
	}
	result["trace"] = json.RawMessage(raw)
}
