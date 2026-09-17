package memory

import (
	"encoding/json"
	"fmt"
	"math"

	"github.com/JBailes/aimee/server-go/bus"
)

// commandLimit preserves the public API's integer conversion and bounded defaults.
func (args commandArgs) limit(name string, fallback, maximum int) int {
	n, ok := args.number(name)
	if !ok || n < 1 {
		return fallback
	}
	return int(math.Min(n, float64(maximum)))
}

func (args commandArgs) stringValue(name string) (string, bool) {
	var value string
	raw, ok := args[name]
	valid := ok && string(raw) != "null" && json.Unmarshal(raw, &value) == nil
	return value, valid
}

func commandScope(args commandArgs, request *DataRequest) bool {
	var scoped bool
	_ = json.Unmarshal(args["scope_context"], &scoped)
	request.IncludeAll = !scoped
	if scoped {
		request.Workspace, request.Project = args.stringOr("workspace", ""), args.stringOr("project", "")
		_ = json.Unmarshal(args["include_all"], &request.IncludeAll)
	}
	return scoped
}

func handleDomainCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	switch verb {
	case "entity_profile", "entity_edges":
		var ok bool
		request.Entity, ok = args.stringValue("entity")
		if !ok || request.Entity == "" {
			return invalid("memory." + verb + " requires entity")
		}
		request.Operation = map[string]string{"entity_profile": "entity-profile", "entity_edges": "entity-edges"}[verb]
		request.Limit = args.limit("limit", 10, 64)
		scoped = commandScope(args, &request)
	case "search_graph", "search_graph_as_of":
		var ok bool
		request.Query, ok = args.stringValue("query")
		if !ok {
			return invalid("memory." + verb + " requires query")
		}
		if verb == "search_graph_as_of" {
			request.AsOf, ok = args.stringValue("as_of")
			if !ok {
				return invalid("memory.search_graph_as_of requires as_of")
			}
		}
		request.Operation, request.Limit = "relation-search", args.limit("limit", 10, 64)
		scoped = commandScope(args, &request)
	case "get_episode":
		request.Key = args.stringOr("episode_key", "")
		if request.Key == "" {
			return invalid("memory.get_episode requires episode_key")
		}
		request.Operation = "episode-get"
	case "get_provenance", "link_query":
		var ok bool
		request.ID, ok = args.positiveID("memory_id")
		if !ok {
			return invalid("missing memory_id")
		}
		if verb == "get_provenance" {
			request.Operation, request.Limit = "provenance-list", args.limit("max", 64, 64)
		} else {
			request.Operation, request.Limit = "link-query", args.limit("max", 32, 64)
		}
	case "link_create":
		var sourceOK, targetOK bool
		request.SourceID, sourceOK = args.positiveID("source_id")
		request.TargetID, targetOK = args.positiveID("target_id")
		request.Relation = args.stringOr("relation", "")
		if !sourceOK || !targetOK || request.SourceID == request.TargetID || request.Relation == "" {
			return invalid("missing source_id/target_id/relation")
		}
		request.Operation = "link-create"
	case "link_delete":
		var ok bool
		request.ID, ok = args.positiveID("link_id")
		if !ok {
			return invalid("missing link_id")
		}
		request.Operation = "link-delete"
	case "list_conflicts":
		request.Operation, request.Limit = "conflict-list", args.limit("max", 64, 256)
	case "query_health":
		request.Operation = "health"
	case "stats_dashboard":
		request.Operation = "stats-dashboard"
	case "stats":
		request.Operation = "stats"
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
	encoded, err := json.Marshal(request)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	data, status := handleData(options, invocation, encoded)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(data, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	result := map[string]any{"status": "ok"}
	switch verb {
	case "get_episode":
		if len(response.Episodes) == 0 {
			return commandResult(commandError("not_found", "episode not found"))
		}
		result["episode"] = response.Episodes[0]
	case "entity_profile":
		if response.EntityProfile == nil {
			return nil, bus.ModuleStatusInternal
		}
		result["profile"] = response.EntityProfile
	case "entity_edges", "search_graph", "search_graph_as_of":
		rows := make([]map[string]any, 0, len(response.Relations))
		for _, r := range response.Relations {
			rows = append(rows, map[string]any{"id": r.ID, "memory_id": r.MemoryID, "episode_id": r.EpisodeID,
				"src_entity": r.Source, "relation": r.Relation, "dst_entity": r.Target, "fact_text": r.Fact,
				"valid_at": r.ValidAt, "invalid_at": r.InvalidAt, "weight": r.Weight, "created_at": r.CreatedAt})
		}
		key := "relations"
		if verb == "entity_edges" {
			key = "edges"
		}
		result[key] = rows
	case "get_provenance":
		rows := response.Provenance
		if rows == nil {
			rows = []Provenance{}
		}
		result["entries"] = rows
	case "link_query":
		rows := make([]map[string]any, 0, len(response.Links))
		for _, r := range response.Links {
			rows = append(rows, map[string]any{"id": r.ID, "source_id": r.SourceID, "target_id": r.TargetID, "relation": r.Relation, "created_at": r.CreatedAt})
		}
		result["links"] = rows
	case "list_conflicts":
		rows := make([]map[string]any, 0, len(response.Conflicts))
		for _, r := range response.Conflicts {
			resolved := 0
			if r.Resolved {
				resolved = 1
			}
			rows = append(rows, map[string]any{"id": r.ID, "memory_a": r.MemoryAID, "memory_b": r.MemoryBID, "detected_at": r.DetectedAt, "resolved": resolved, "resolution": r.Resolution})
		}
		result["conflicts"] = rows
	case "query_health":
		if response.Health == nil {
			return nil, bus.ModuleStatusInternal
		}
		health, _ := json.Marshal(response.Health)
		var fields map[string]any
		if json.Unmarshal(health, &fields) != nil {
			return nil, bus.ModuleStatusInternal
		}
		fields["write_to_readable_lag"] = map[string]any{"samples": 0, "state": "unmeasured"}
		result["health"] = fields
	case "stats_dashboard":
		if len(response.Payload) == 0 {
			return nil, bus.ModuleStatusInternal
		}
		result["dashboard"] = response.Payload
	case "stats":
		s := response.Stats
		if s == nil {
			return nil, bus.ModuleStatusInternal
		}
		tiers := make([]int, 6)
		for i := range tiers {
			tiers[i] = s.TierCounts[fmt.Sprintf("L%d", i)]
		}
		names := []string{"fact", "preference", "decision", "episode", "task", "scratch", "procedure", "policy", "workflow", "opinion"}
		kinds := make([]int, len(names))
		for i, name := range names {
			kinds[i] = s.KindCounts[name]
		}
		result["stats"] = map[string]any{"total": s.Total, "conflicts": s.Conflicts, "tier_counts": tiers, "kind_counts": kinds,
			"pagerank_last_ms": 0, "pagerank_avg_ms": 0, "pagerank_max_ms": 0, "pagerank_samples": 0, "pagerank_last_candidates": 0, "pagerank_last_edges": 0}
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}
