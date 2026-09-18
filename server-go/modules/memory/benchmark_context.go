package memory

import (
	"encoding/json"
	"fmt"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

type benchmarkContext struct {
	Status  string `json:"status"`
	Context string `json:"context"`
	Tokens  int    `json:"tokens"`
	Kept    int    `json:"kept"`
}

// Keep the benchmark's byte/4 estimate and numbered snippets, while enforcing
// both budgets on the complete lines, including numbering, ellipses and newlines.
func buildBenchmarkContext(rows []Record, topK, tokenBudget, capacity int) benchmarkContext {
	result := benchmarkContext{Status: "ok"}
	var out strings.Builder
	perItem := tokenBudget / 2
	if perItem < 96 {
		perItem = tokenBudget
	}
	for _, row := range rows {
		if result.Kept >= topK {
			break
		}
		prefix := fmt.Sprintf("[%d] ", result.Kept+1)
		budget := min(perItem*4, (tokenBudget-result.Tokens)*4, capacity-1-out.Len()) - len(prefix) - 1
		if budget <= 0 || row.Content == "" {
			continue
		}
		content := row.Content
		if len(content) > budget {
			if budget <= 3 {
				continue
			}
			limit := budget - 3
			for limit > 0 && !utf8.RuneStart(content[limit]) {
				limit--
			}
			if limit == 0 {
				continue
			}
			content = content[:limit] + "..."
		}
		line := prefix + content + "\n"
		out.WriteString(line)
		result.Tokens += (len(line) + 3) / 4
		result.Kept++
	}
	result.Context = out.String()
	return result
}

func handleBenchmarkContext(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	query, ok := args.stringValue("query")
	topK, budget, capacity := args.integer("top_k", 10), args.integer("token_budget", 2000), args.integer("capacity", 16384)
	if topK <= 0 {
		topK = 10
	}
	if budget <= 0 {
		budget = 2000
	}
	topK = min(topK, 32)
	budget = min(budget, 131072)
	if !ok || capacity < 1 || capacity > 512*1024 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	request := DataRequest{Operation: "search", Query: query, Limit: min(32, topK*4), Project: args.stringOr("project", ""), Workspace: args.stringOr("workspace", ""), IncludeAll: args.boolean("include_all")}
	if raw, ok := args["scope"]; ok && json.Unmarshal(raw, &request.Scope) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	raw, _ := json.Marshal(request)
	encoded, status := handleData(options, invocation, raw)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(encoded, &response) != nil {
		return nil, bus.ModuleStatusInternal
	}
	return commandResult(buildBenchmarkContext(response.Records, topK, budget, capacity))
}
