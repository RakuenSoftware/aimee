package memory

import (
	"encoding/json"
	"fmt"
	"strings"

	"github.com/JBailes/aimee/server-go/bus"
)

// Preserve the canonical MCP filter priority while keeping memory scope policy
// with its Go owner. "any" and "current" use the resolved request context.
func searchViewScope(args commandArgs) (Scope, bool) {
	var filter struct {
		Scope commandArgs `json:"scope"`
	}
	if json.Unmarshal(args["filter"], &filter) != nil {
		return Scope{}, false
	}
	for _, kind := range []string{ScopeWorkspace, ScopeProject, "session", ScopeUser} {
		value := filter.Scope.stringOr(string(kind), "")
		if value != "" && value != "any" && value != "current" {
			return Scope{Type: kind, Value: value}, true
		}
	}
	return Scope{}, false
}

func memorySearchText(query string, records []publicMemoryRecord, missing bool) string {
	var out strings.Builder
	if missing {
		out.WriteString("Active project context is unavailable; showing shared/global memory only.\n\n")
	}
	if len(records) == 0 {
		fmt.Fprintf(&out, "No facts found for '%s'", query)
	} else {
		fmt.Fprintf(&out, "Found %d fact(s):\n\n", len(records))
		for _, r := range records {
			fmt.Fprintf(&out, "- **%s** [%s/%s]: %s\n", r.Key, r.Tier, r.Kind, r.Content)
		}
	}
	return out.String()
}

// The local operator probe uses the same Go retrieval owner and deployment
// setting as the serving instance. It never overrides fusion policy.
func handleFusionProbe(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	query, ok := args.stringValue("query")
	if !ok || strings.TrimSpace(query) == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	state, status := handleData(options, invocation, []byte(`{"operation":"fusion-state-get"}`))
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var configured DataResponse
	if json.Unmarshal(state, &configured) != nil || configured.Allowed == nil {
		return nil, bus.ModuleStatusInternal
	}
	// The former local data-stage request had no active scope context. Preserve
	// that visibility; an operator probe must not turn on include_all implicitly.
	request, _ := json.Marshal(DataRequest{Operation: "search", Query: query, Limit: 20})
	encoded, status := handleData(options, invocation, request)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(encoded, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	mode := "off"
	if *configured.Allowed {
		mode = "on"
	}
	var out strings.Builder
	fmt.Fprintf(&out, "fusion=%s (instance setting), results=%d\n", mode, len(response.Records))
	for i, row := range response.Records {
		fmt.Fprintf(&out, "  #%-2d id=%-8d %s\n", i+1, row.ID, row.Key)
	}
	return commandResult(map[string]any{"status": "ok", "output": out.String()})
}
