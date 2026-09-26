package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"strconv"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Public records retain the KB response shape, without the former native
// fixed-size content buffer. Enrichment refuses changed payloads and mismatched
// observed versions; a scoped READ COMMITTED transaction alone is not a snapshot.
type publicMemoryRecord struct {
	Version *MemoryRecordVersion `json:"version,omitempty"`

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
 (SELECT id,scope,summary FROM memory_summaries summary WHERE summary.memory_id=m.id AND `+summaryCurrentInputsSQL("summary", "m")+` ORDER BY id LIMIT 4) summaries
 ORDER BY CASE WHEN scope='headline' AND summary<>'' THEN 0 ELSE 1 END,id LIMIT 1),''),
m.scope_type,m.scope_value,m.tier,m.kind,m.key,m.content,m.confidence,
m.record_revision::text,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1),
(`+currentMemorySQL("m.")+`),(`+historicalMemoryInspectionSQL("m.")+`)
FROM memories m WHERE m.id=ANY($1::text::bigint[])`, memoryIDsParameter(ids))
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	type observedMetadata struct {
		publicMemoryRecord
		scope           Scope
		revision, owner string
		current         bool
		historical      bool
	}
	metadata := make(map[int64]observedMetadata, len(records))
	for rows.Next() {
		var r observedMetadata
		if err = rows.Scan(&r.ID, &r.UseCases, &r.UseCount, &r.LastUsedAt, &r.CreatedAt, &r.UpdatedAt, &r.SourceSession, &r.ProvenanceCategory, &r.Headline, &r.scope.Type, &r.scope.Value, &r.Tier, &r.Kind, &r.Key, &r.Content, &r.Confidence, &r.revision, &r.owner, &r.current, &r.historical); err != nil {
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
			return nil, fmt.Errorf("memory: metadata missing for record %d", record.ID)
		}
		if r.scope != record.Scope || r.Tier != record.Tier || r.Kind != record.Kind || r.Key != record.Key || r.Content != record.Content || r.Confidence != record.Confidence {
			return nil, fmt.Errorf("memory: record %d changed during public enrichment", record.ID)
		}
		if record.currentRead && !r.current {
			return nil, fmt.Errorf("memory: record %d is no longer current during public enrichment", record.ID)
		}
		if record.historicalRead && !r.historical {
			return nil, fmt.Errorf("memory: record %d is no longer inspectable during public enrichment", record.ID)
		}
		v := record.Version
		if v == nil {
			v = record.observedVersion
		}
		if v != nil && (!v.validFor(record.ID) || v.RecordRevision != r.revision || v.OwnerID != r.owner) {
			return nil, fmt.Errorf("memory: record %d version changed during public enrichment", record.ID)
		}
		r.Version = record.Version
		r.Tier, r.Kind, r.Key, r.Content, r.Confidence = record.Tier, record.Kind, record.Key, record.Content, record.Confidence
		result = append(result, r.publicMemoryRecord)
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
			if args.stringOr("format", "") == "mcp" {
				request.IncludeAll = args.boolean("include_all")
				if request.Workspace == "__aimee_scope_missing__" {
					request.Workspace = ""
				}
				if request.Project == "__aimee_scope_missing__" {
					request.Project = ""
				}
				if scope, explicit := searchViewScope(args); explicit {
					if _, err := normalizeScope(PlacementKB, scope); err != nil {
						return invalid(err.Error())
					}
					request.Operation, request.Scope, request.IncludeAll = "search", scope, false
					scoped = false
				}
			}
		} else {
			request.Operation = "search"
			request.Scope = Scope{Type: args.stringOr("scope_type", ""), Value: args.stringOr("scope_value", "")}
			if _, err := normalizeScope(PlacementKB, request.Scope); err != nil {
				return invalid(err.Error())
			}
		}
	case "get":
		if raw, exists := args["include_version"]; exists {
			if string(raw) == "null" || json.Unmarshal(raw, &request.IncludeVersion) != nil {
				return invalid("include_version must be boolean")
			}
		}
		var ok bool
		if request.IncludeVersion && (args.stringOr("view", "") == "session" || (args.stringOr("view", "") == "console" && args.stringOr("format", "json") != "json")) {
			return commandResult(commandError("unsupported_mode", "include_version requires a JSON record view"))
		}
		request.ReadPolicy, ok = commandReadPolicy(args)
		if !ok {
			return invalid("read_policy must be a versioned object with recognized fields")
		}
		request.ID, ok = args.decimalID("id")
		if !ok {
			return invalid("memory.get requires a positive integer id")
		}
		request.Operation, request.AsOf = "get", args.stringOr("as_of", "")
		scoped = commandScope(args, &request)
	case "list":
		request.Operation, request.Tier, request.Kind, request.Limit = "list", args.stringOr("tier", ""), args.stringOr("kind", ""), args.limit("limit", 20, 64)
		scoped = commandScope(args, &request)
		if args.stringOr("format", "") == "wiki" {
			request.Operation, request.PublicView = "wiki-bundle", false
		}
	case "fact_history":
		var ok bool
		request.Key, ok = args.stringValue("key")
		if !ok {
			return invalid("memory.fact_history requires key")
		}
		request.Operation, request.Limit = "fact-history", args.limit("max", 16, 64)
		scoped = commandScope(args, &request)
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
			if args.stringOr("view", "") == "session" {
				request.Pattern = sessionSearchKeyword(request.Pattern)
			}
		}
		scoped = commandScope(args, &request)
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
	if response.Read != nil && response.Read.ErrorCode != "" {
		return commandResult(commandError(response.Read.ErrorCode, response.Read.Message))
	}
	if args.stringOr("view", "") == "session" {
		return sessionMemoryView(options, invocation, args, request, response.PublicRecords)
	}
	if verb == "list" && args.stringOr("format", "") == "mcp" {
		missing := scoped && !request.IncludeAll && request.Workspace == "" && request.Project == ""
		var text strings.Builder
		if missing {
			text.WriteString("Active project context is unavailable; showing shared/global memory only.\n\n")
		}
		if len(response.PublicRecords) == 0 {
			text.WriteString("No L2 facts stored.")
		} else {
			fmt.Fprintf(&text, "%d fact(s):\n\n", len(response.PublicRecords))
			for _, record := range response.PublicRecords {
				fmt.Fprintf(&text, "- **%s**: %s\n", record.Key, record.Content)
			}
		}
		return commandResult(map[string]any{"status": "ok", "text": text.String(), "active_context_missing": missing})
	}
	if verb == "find_facts" && args.stringOr("format", "") == "tool" {
		return commandResult(map[string]any{"status": "ok", "count": len(response.PublicRecords),
			"text": memorySearchText(request.Query, response.PublicRecords, false)})
	}
	if args.stringOr("view", "") == "console" && (verb == "get" || verb == "list") {
		if verb == "get" {
			if len(response.PublicRecords) == 0 {
				return commandResult(commandError("not_found", "memory not found"))
			}
			return inspectionOutput(consoleMemoryRecord(response.PublicRecords[0]), "", args)
		}
		rows := make([]map[string]any, 0, len(response.PublicRecords))
		for _, r := range response.PublicRecords {
			rows = append(rows, consoleMemoryRecord(r))
		}
		return inspectionOutput(rows, "", args)
	}
	if verb == "fact_history" && (args.stringOr("view", "") == "console" || args.stringOr("format", "") == "mcp") {
		return historyInspection(response.PublicRecords, args)
	}
	if request.Operation == "wiki-bundle" {
		if len(response.Payload) == 0 {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(response.Payload)
	}
	if verb == "find_facts_visible" && args.stringOr("format", "") == "mcp" {
		missing := scoped && !request.IncludeAll && request.Workspace == "" && request.Project == ""
		result := map[string]any{"status": "ok", "text": memorySearchText(request.Query, response.PublicRecords, missing), "active_context_missing": missing}
		if response.RetrievalCapabilities != nil {
			result["retrieval_capabilities"] = response.RetrievalCapabilities
		}
		return commandResult(result)
	}
	result := map[string]any{"status": "ok"}
	if response.RetrievalCapabilities != nil {
		result["retrieval_capabilities"] = response.RetrievalCapabilities
	}
	if args.stringOr("view", "") == "server" {
		result["store"] = "kb"
	}
	if verb == "get" {
		if len(response.PublicRecords) == 0 {
			return commandResult(commandError("not_found", "memory not found"))
		}
		result["memory"] = response.PublicRecords[0]
		if response.Read != nil {
			result["read"] = response.Read
		}
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

// Preserve the native console's record schema while keeping its contents and
// integer IDs in the owner. The host transports the rendered output as a string.
func consoleMemoryRecord(r publicMemoryRecord) map[string]any {
	result := map[string]any{
		"id": r.ID, "tier": r.Tier, "kind": r.Kind, "key": r.Key, "content": r.Content,
		"confidence": r.Confidence, "use_count": r.UseCount, "last_used_at": r.LastUsedAt,
		"created_at": r.CreatedAt, "updated_at": r.UpdatedAt, "source_session": r.SourceSession,
		"provenance_category": r.ProvenanceCategory,
	}
	if r.Version != nil {
		result["version"] = r.Version
	}
	return result
}

func historyInspection(records []publicMemoryRecord, args commandArgs) ([]byte, bus.ModuleStatus) {
	rows := make([]map[string]any, 0, len(records))
	mcp := args.stringOr("format", "") == "mcp"
	for _, r := range records {
		row := map[string]any{"id": r.ID, "tier": r.Tier, "kind": r.Kind, "content": r.Content, "confidence": r.Confidence, "updated_at": r.UpdatedAt}
		if !mcp {
			row = consoleMemoryRecord(r)
		}
		rows = append(rows, row)
	}
	if mcp {
		status := "ok"
		if len(rows) == 0 {
			status = "empty"
		}
		raw, err := json.Marshal(map[string]any{"status": status, "count": len(rows), "history": rows})
		if err != nil {
			return nil, bus.ModuleStatusInternal
		}
		return commandResult(map[string]any{"status": "ok", "output": string(raw)})
	}
	return inspectionOutput(rows, "", args)
}
