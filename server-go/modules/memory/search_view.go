package memory

import (
	"encoding/json"
	"fmt"
	"strings"
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
