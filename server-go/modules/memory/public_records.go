package memory

import (
	"context"
	"encoding/json"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Public records retain the KB response shape, without the former native
// fixed-size content buffer. Metadata is read inside the same scoped transaction.
type publicMemoryRecord struct {
	ID                 int64   `json:"id"`
	Tier               string  `json:"tier"`
	Kind               string  `json:"kind"`
	Key                string  `json:"key"`
	Headline           string  `json:"headline"`
	Content            string  `json:"content"`
	UseCases           string  `json:"use_cases"`
	Confidence         float64 `json:"confidence"`
	UseCount           int     `json:"use_count"`
	LastUsedAt         string  `json:"last_used_at"`
	CreatedAt          string  `json:"created_at"`
	UpdatedAt          string  `json:"updated_at"`
	SourceSession      string  `json:"source_session"`
	ProvenanceCategory string  `json:"provenance_category"`
	RetrievalScore     float64 `json:"retrieval_score"`
	HybridRank         int     `json:"hybrid_rank"`
}

func (s *postgresDataStore) publicRecords(ctx context.Context, records []Record) ([]publicMemoryRecord, error) {
	result := make([]publicMemoryRecord, 0, len(records))
	if len(records) == 0 {
		return result, nil
	}
	ids := make([]int64, len(records))
	for i, r := range records {
		ids[i] = r.ID
	}
	rows, err := s.db.Query(ctx, `SELECT m.id,COALESCE(m.use_cases,''),m.use_count,
COALESCE(m.last_used_at,''),m.created_at,m.updated_at,COALESCE(m.source_session,''),
COALESCE(m.provenance_category,''),COALESCE((SELECT summary FROM
 (SELECT id,scope,summary FROM memory_summaries WHERE memory_id=m.id ORDER BY id LIMIT 4) summaries
 ORDER BY CASE WHEN scope='headline' AND summary<>'' THEN 0 ELSE 1 END,id LIMIT 1),'')
FROM memories m WHERE m.id=ANY($1::text::bigint[])`, memoryIDsParameter(ids))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	metadata := make(map[int64]publicMemoryRecord, len(records))
	for rows.Next() {
		var r publicMemoryRecord
		if err = rows.Scan(&r.ID, &r.UseCases, &r.UseCount, &r.LastUsedAt, &r.CreatedAt, &r.UpdatedAt, &r.SourceSession, &r.ProvenanceCategory, &r.Headline); err != nil {
			return nil, err
		}
		metadata[r.ID] = r
	}
	if err = rows.Err(); err != nil {
		return nil, err
	}
	for _, record := range records {
		r, ok := metadata[record.ID]
		if !ok {
			continue
		}
		r.Tier, r.Kind, r.Key, r.Content, r.Confidence = record.Tier, record.Kind, record.Key, record.Content, record.Confidence
		result = append(result, r)
	}
	return result, nil
}

// The store bus transports scalars; PostgreSQL expands this bounded, numeric
// array parameter. Values never become part of the SQL statement.
func memoryIDsParameter(ids []int64) string {
	items := make([]string, len(ids))
	for i, id := range ids {
		items[i] = strconv.FormatInt(id, 10)
	}
	return "{" + strings.Join(items, ",") + "}"
}

func handleRecordCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true, PublicView: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "find_facts":
		var ok bool
		request.Query, ok = args.stringValue("query")
		if !ok {
			return invalid("memory.find_facts requires query")
		}
		request.Operation, request.Limit = "adaptive-search", args.limit("limit", 20, 64)
		_, explicit := args.number("limit")
		request.AutomaticLimit = !explicit
		scoped = commandScope(args, &request)
	case "find_facts_visible", "find_facts_scoped":
		var ok bool
		request.Query, ok = args.stringValue("query")
		if !ok {
			return invalid("memory." + verb + " requires query")
		}
		request.Limit, request.IncludeAll = args.limit("limit", 20, 64), false
		if verb == "find_facts_visible" {
			request.Operation = "visible-search"
			request.Workspace, request.Project = args.stringOr("workspace", ""), args.stringOr("project", "")
			scoped = true
		} else {
			request.Operation = "search"
			request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
			if _, err := normalizeScope(PlacementKB, request.Scope); err != nil {
				return invalid(err.Error())
			}
		}
	case "get":
		var ok bool
		request.ID, ok = args.positiveID("id")
		if !ok {
			return invalid("memory.get requires a positive integer id")
		}
		request.Operation, request.AsOf = "get", args.stringOr("as_of", "")
		scoped = commandScope(args, &request)
	case "list":
		request.Operation, request.Tier, request.Kind, request.Limit = "list", args.stringOr("tier", ""), args.stringOr("kind", ""), args.limit("limit", 20, 64)
		scoped = commandScope(args, &request)
	case "fact_history":
		var ok bool
		request.Key, ok = args.stringValue("key")
		if !ok {
			return invalid("memory.fact_history requires key")
		}
		request.Operation, request.Limit = "fact-history", args.limit("max", 16, 64)
	case "top_l2_facts", "load_eval_corpus", "list_session_scope_priority", "list_session_scope_priority_like", "search_facts_patterns_by_keyword":
		request.Operation = "query-records"
		switch verb {
		case "top_l2_facts":
			request.Mode, request.Limit = "top-l2", args.limit("max", 5, 64)
		case "load_eval_corpus":
			request.Mode, request.Limit = "eval", args.limit("max", 100, 100)
			if n, ok := args.number("max"); ok && n < 1 {
				request.Limit = 20
			}
		case "list_session_scope_priority":
			request.Mode, request.Limit = "session-priority", args.limit("max", 24, 64)
		case "list_session_scope_priority_like":
			var ok bool
			request.Pattern, ok = args.stringValue("pattern")
			if !ok {
				return invalid("memory.list_session_scope_priority_like requires pattern")
			}
			request.Mode, request.Limit = "session-priority", args.limit("max", 5, 64)
		case "search_facts_patterns_by_keyword":
			var ok bool
			request.Pattern, ok = args.stringValue("keyword")
			if !ok {
				return invalid("memory.search_facts_patterns_by_keyword requires keyword")
			}
			request.Mode, request.Limit = "facts-patterns", args.limit("max", 5, 64)
		}
		if verb != "load_eval_corpus" {
			scoped = commandScope(args, &request)
		}
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		if status == bus.ModuleStatusInternal || status == bus.ModuleStatusCapabilityAbsent {
			return commandResult(commandError("unavailable", "memory module unavailable"))
		}
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok"}
	if verb == "get" {
		if len(response.PublicRecords) == 0 {
			return commandResult(commandError("not_found", "memory not found"))
		}
		result["memory"] = response.PublicRecords[0]
		if request.AsOf != "" {
			result["as_of"] = request.AsOf
			result["valid_at"] = "unknown"
			if response.ValidAt != nil {
				result["valid_at"] = *response.ValidAt
			}
		}
	} else {
		key := "memories"
		if verb == "fact_history" {
			key = "history"
		}
		if verb == "find_facts" || verb == "find_facts_visible" || verb == "find_facts_scoped" {
			key = "facts"
		}
		if response.PublicRecords == nil {
			response.PublicRecords = []publicMemoryRecord{}
		}
		result[key] = response.PublicRecords
		if verb == "load_eval_corpus" {
			result["label"] = ""
			if len(response.PublicRecords) > 0 {
				result["label"] = "durable L1-L3"
			}
		}
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
