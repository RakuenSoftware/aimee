package memory

import (
	"encoding/json"
	"fmt"
	"sort"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// Hosts supply a remaining byte budget. Selection, fallback and memory-specific
// rendering belong to this owner; a host never reconstructs or re-ranks rows.
func sessionMemoryView(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs, request DataRequest, records []publicMemoryRecord) ([]byte, bus.ModuleStatus) {
	budget := args.limit("budget_bytes", 8192, 1<<20)
	if n, ok := args.number("budget_bytes"); ok && n == 0 {
		budget = 0
	}
	section := args.stringOr("section", "")
	header, limit := "", 0
	switch section {
	case "facts":
		header, limit = "# Key Facts", 15
	case "project":
		header, limit = "# Workspace Context", 10
		if request.Project != "" {
			header = "# Project Context"
		}
	case "shared":
		header, limit = "# Shared Context", 5
	case "relevant":
		header, limit = "# Relevant Context", 5
	default:
		return commandResult(commandError("invalid_argument", "unknown session memory section"))
	}
	if section == "project" || section == "shared" {
		rankRequest := DataRequest{Operation: "scope-rank", Workspace: request.Workspace, Project: request.Project}
		for _, r := range records {
			rankRequest.IDs = append(rankRequest.IDs, r.ID)
		}
		ranks := make(map[int64]int)
		if len(records) > 0 {
			raw, _ := json.Marshal(rankRequest)
			body, status := handleData(options, invocation, raw)
			if status != bus.ModuleStatusOK {
				return nil, status
			}
			var result DataResponse
			if json.Unmarshal(body, &result) != nil || len(result.ScopeRanks) != len(records) {
				return nil, bus.ModuleStatusInternal
			}
			for _, r := range result.ScopeRanks {
				ranks[r.ID] = r.Rank
			}
		}
		selected := make([]publicMemoryRecord, 0, len(records))
		for _, r := range records {
			rank := ranks[r.ID]
			if (section == "project" && rank >= 2 && rank <= 3) || (section == "shared" && rank == 1) {
				selected = append(selected, r)
			}
		}
		sort.SliceStable(selected, func(i, j int) bool { return ranks[selected[i].ID] > ranks[selected[j].ID] })
		records = selected
		if section == "project" && len(records) == 0 && request.Project != "" {
			fallback := request
			fallback.Pattern, fallback.Limit = "%"+request.Project+"%", 5
			raw, _ := json.Marshal(fallback)
			body, status := handleData(options, invocation, raw)
			if status != bus.ModuleStatusOK {
				return nil, status
			}
			var result DataResponse
			if json.Unmarshal(body, &result) != nil {
				return nil, bus.ModuleStatusInternal
			}
			records = result.PublicRecords
			header = fmt.Sprintf("# Project Context (%s)", request.Project)
		}
	}
	var out strings.Builder
	count := 0
	for _, r := range records {
		if count >= limit {
			break
		}
		var line string
		switch section {
		case "facts":
			line = fmt.Sprintf("- %s: %s\n", r.Key, sessionExcerpt(r.Content, 300))
		case "relevant":
			line = fmt.Sprintf("- %s: %s\n", r.Key, r.Content)
		default:
			text := r.Content
			if text == "" {
				text = r.Key
			}
			if r.Kind != "" {
				line = fmt.Sprintf("- [%s] %s\n", r.Kind, sessionExcerpt(text, 300))
			} else {
				line = "- " + sessionExcerpt(text, 300) + "\n"
			}
		}
		prefix := ""
		if count == 0 {
			prefix = header + "\n"
		}
		if out.Len()+len(prefix)+len(line)+1 > budget {
			break
		}
		out.WriteString(prefix)
		out.WriteString(line)
		count++
	}
	if count > 0 {
		out.WriteByte('\n')
	}
	return commandResult(map[string]any{"status": "ok", "text": out.String(), "count": count})
}

func sessionExcerpt(text string, limit int) string {
	if len(text) <= limit {
		return text
	}
	for limit > 0 && !utf8.RuneStart(text[limit]) {
		limit--
	}
	return text[:limit]
}

func sessionSearchKeyword(prompt string) string {
	p := prompt
	for strings.HasPrefix(p, " ") || strings.HasPrefix(p, "Check ") || strings.HasPrefix(p, "Deploy ") || strings.HasPrefix(p, "Verify ") || strings.HasPrefix(p, "List ") {
		if i := strings.IndexByte(p, ' '); i >= 0 {
			p = strings.TrimLeft(p[i:], " ")
		} else {
			break
		}
	}
	if p == "" {
		return prompt
	}
	return sessionExcerpt(p, 60)
}
