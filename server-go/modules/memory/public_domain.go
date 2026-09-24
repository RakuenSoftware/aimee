package memory

import (
	"encoding/json"
	"fmt"
	"math"
	"strconv"
	"strings"

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

// Decimal strings preserve full int64 IDs across native JSON transports.
// Numeric IDs remain limited to the exactly representable transport range.
func (args commandArgs) decimalID(name string) (int64, bool) {
	if raw, ok := args.stringValue(name); ok {
		id, err := strconv.ParseInt(raw, 10, 64)
		return id, err == nil && id > 0 && strconv.FormatInt(id, 10) == raw
	}
	return args.positiveID(name)
}

func validCommandScopeArgs(args commandArgs) bool {
	for _, key := range []string{"workspace", "project"} {
		if raw, exists := args[key]; exists {
			var value *string
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return false
			}
		}
	}
	for _, key := range []string{"scope_context", "include_all"} {
		if raw, exists := args[key]; exists {
			var value *bool
			if json.Unmarshal(raw, &value) != nil || value == nil {
				return false
			}
		}
	}
	return true
}

func commandScope(args commandArgs, request *DataRequest) bool {
	var scoped bool
	_ = json.Unmarshal(args["scope_context"], &scoped)
	workspace, project := args.stringOr("workspace", ""), args.stringOr("project", "")
	// Legacy callers may supply an audience without the newer context marker.
	// Honor those values instead of silently changing the query to all scopes.
	scoped = scoped || workspace != "" || project != ""
	request.IncludeAll = !scoped
	_ = json.Unmarshal(args["include_all"], &request.IncludeAll)
	scoped = scoped || !request.IncludeAll
	if scoped {
		request.Workspace, request.Project = workspace, project
	}
	return scoped
}

func handleDomainCommand(options handlerOptions, invocation bus.ModuleInvocation, verb string, args commandArgs) ([]byte, bus.ModuleStatus) {
	request := DataRequest{IncludeAll: true}
	scoped := false
	invalid := func(message string) ([]byte, bus.ModuleStatus) {
		return commandResult(commandError("invalid_argument", message))
	}
	consoleLinks := args.stringOr("view", "") == "console" &&
		(verb == "link_create" || verb == "link_query" || verb == "link_delete")
	if consoleLinks {
		if format := args.stringOr("format", "json"); format != "json" && format != "text" {
			return invalid("format must be json or text")
		}
	}
	switch verb {
	case "scene_list":
		request.Operation, request.Limit = "scenes", args.limit("limit", 100, 100)
		scoped = commandScope(args, &request)
	case "scene_show":
		var valid bool
		request.ID, valid = args.positiveID("scene_id")
		if !valid {
			return invalid("missing or invalid scene_id")
		}
		request.Operation, request.Limit = "scene-members", args.limit("limit", 512, 512)
		scoped = commandScope(args, &request)
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
		scoped = commandScope(args, &request)
	case "get_provenance", "link_query":
		var ok bool
		request.ID, ok = args.decimalID("memory_id")
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
		request.SourceID, sourceOK = args.decimalID("source_id")
		request.TargetID, targetOK = args.decimalID("target_id")
		request.Relation = args.stringOr("relation", "")
		if !sourceOK || !targetOK || request.SourceID == request.TargetID || request.Relation == "" {
			return invalid("missing source_id/target_id/relation")
		}
		if consoleLinks {
			switch request.Relation {
			case "supersedes", "depends_on", "contradicts", "related_to":
			default:
				return invalid("relation must be: supersedes, depends_on, contradicts, or related_to")
			}
		}
		request.Operation = "link-create"
	case "link_delete":
		var ok bool
		request.ID, ok = args.decimalID("link_id")
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
	case "scene_list":
		if response.Scenes == nil {
			response.Scenes = []MemoryScene{}
		}
		result["scenes"] = response.Scenes
	case "scene_show":
		if response.SceneMembers == nil {
			response.SceneMembers = []SceneMember{}
		}
		result["scene_id"], result["members"] = request.ID, response.SceneMembers
	case "get_episode":
		if len(response.Episodes) == 0 {
			return commandResult(commandError("not_found", "episode not found"))
		}
		result["episode"] = response.Episodes[0]
	case "entity_profile":
		if response.EntityProfile == nil {
			return commandResult(commandError("not_found", "entity profile not found"))
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
		if args.stringOr("format", "") == "mcp" {
			status := "ok"
			if len(rows) == 0 {
				status = "empty"
			}
			output, err := json.Marshal(map[string]any{"status": status, "count": len(rows), "provenance": rows})
			if err != nil {
				return nil, bus.ModuleStatusInternal
			}
			return commandResult(map[string]any{"status": "ok", "output": string(output)})
		}
	case "link_query":
		rows := make([]map[string]any, 0, len(response.Links))
		for _, r := range response.Links {
			rows = append(rows, map[string]any{"id": r.ID, "source_id": r.SourceID, "target_id": r.TargetID, "relation": r.Relation, "created_at": r.CreatedAt})
		}
		result["links"] = rows
		if consoleLinks {
			var output strings.Builder
			if len(response.Links) == 0 {
				fmt.Fprintf(&output, "No links for memory %d\n", request.ID)
			}
			for _, link := range response.Links {
				direction, other := "->", link.TargetID
				if link.SourceID != request.ID {
					direction, other = "<-", link.SourceID
				}
				fmt.Fprintf(&output, "  [%d] %s [%s] %d  (%s)\n", link.ID, direction, link.Relation, other, link.CreatedAt)
			}
			return inspectionOutput(rows, output.String(), args)
		}
	case "link_create":
		if consoleLinks {
			return inspectionOutput(result, fmt.Sprintf("Linked memory %d -[%s]-> %d\n", request.SourceID, request.Relation, request.TargetID), args)
		}
	case "link_delete":
		if consoleLinks {
			return inspectionOutput(result, fmt.Sprintf("Deleted link %d\n", request.ID), args)
		}
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
		if args.stringOr("view", "") == "console" {
			h := response.Health
			result["text"] = fmt.Sprintf("Memory Health (last 7 days, %d cycles):\n"+
				"  Contradiction rate: %.1f%% (%d detected)\n"+
				"  Promotion rate:     %.1f%% (%d promoted)\n"+
				"  Demotion rate:      %.1f%% (%d demoted)\n"+
				"  Staleness:          %.1f%% of L2 facts unused in 30+ days\n"+
				"  Write-to-readable:  unmeasured\n"+
				"  Expirations:        %d\n", h.Cycles, h.ContradictionRate*100, h.Contradictions,
				h.PromotionRate*100, h.Promotions, h.DemotionRate*100, h.Demotions, h.Staleness*100, h.Expirations)
		}
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
		timing := pageRankMetricState.snapshot()
		result["stats"] = map[string]any{"total": s.Total, "conflicts": s.Conflicts, "tier_counts": tiers, "kind_counts": kinds,
			"pagerank_last_ms": timing.LastMS, "pagerank_avg_ms": timing.AverageMS, "pagerank_max_ms": timing.MaximumMS, "pagerank_samples": timing.Samples, "pagerank_last_candidates": timing.Candidates, "pagerank_last_edges": timing.Edges}
		if args.stringOr("view", "") == "console" {
			addStatsConsole(result, *s, timing)
			// The historical CLI includes effectiveness only in JSON output and
			// treats this secondary query as optional. Never hide a primary stats failure.
			var effectiveness bool
			_ = json.Unmarshal(args["effectiveness"], &effectiveness)
			if effectiveness {
				data, status := handleData(options, invocation, []byte(`{"operation":"effectiveness-stats","include_all":true}`))
				var extra DataResponse
				if status == bus.ModuleStatusOK && json.Unmarshal(data, &extra) == nil && extra.Effectiveness != nil {
					e := extra.Effectiveness
					result["display"].(map[string]any)["effectiveness"] = map[string]any{
						"avg_effectiveness": e.Average, "low_effectiveness": e.LowCount,
						"high_impact": e.HighImpactCount, "never_surfaced_l2": e.NeverSurfacedL2}
				}
			}
		}
	}
	if scoped {
		result["active_context_missing"] = request.Workspace == "" && request.Project == ""
	}
	return commandResult(result)
}

// Keep all stats projections on one snapshot from the Go PageRank scorer.
// No samples means unmeasured; these counters do not claim retrieval activation.
func addStatsConsole(result map[string]any, stats MemoryStats, timing pageRankMetrics) {
	tiers := make(map[string]int, 6)
	for i := 0; i < 6; i++ {
		name := fmt.Sprintf("L%d", i)
		tiers[name] = stats.TierCounts[name]
	}
	result["display"] = map[string]any{"total": stats.Total, "conflicts": stats.Conflicts, "tiers": tiers, "pagerank": timing}
	state := "measured"
	if timing.Samples == 0 {
		state = "unmeasured"
	}
	result["pagerank_timing"] = map[string]any{"elapsed_ms": timing.LastMS, "avg_ms": timing.AverageMS, "max_ms": timing.MaximumMS, "samples": timing.Samples, "candidates": timing.Candidates, "edges": timing.Edges, "state": state, "source": "go-pagerank", "recall_samples": timing.RecallSamples, "candidate_samples": timing.CandidateSamples}
	result["pagerank_text"] = fmt.Sprintf("PageRank: elapsed=%.3fms avg=%.3fms max=%.3fms samples=%d candidates=%d edges=%d\n", timing.LastMS, timing.AverageMS, timing.MaximumMS, timing.Samples, timing.Candidates, timing.Edges)
	result["text"] = fmt.Sprintf("Memory Stats:\n  Total:              %d\n  Conflicts:          %d\n"+
		"  Tiers:              L0=%d L1=%d L2=%d L3=%d L4=%d L5=%d\n"+
		"  PageRank latency:   last=%.3fms avg=%.3fms max=%.3fms samples=%d candidates=%d edges=%d\n",
		stats.Total, stats.Conflicts, tiers["L0"], tiers["L1"], tiers["L2"], tiers["L3"], tiers["L4"], tiers["L5"], timing.LastMS, timing.AverageMS, timing.MaximumMS, timing.Samples, timing.Candidates, timing.Edges)
}
