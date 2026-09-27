package memory

import (
	"context"
	"encoding/json"
	"fmt"
	"os"
	"path/filepath"
	"strings"
	"unicode/utf8"

	"github.com/JBailes/aimee/server-go/bus"
)

// Rule collections are global; hidden project activity never changes this proof.
const ruleCollectionRevisionSQL = `(SELECT (o.rules_revision+COALESCE((SELECT sum(generation)
 FROM (SELECT generation FROM memory_collection_generations WHERE scope_type='global' AND scope_value='_global'
 UNION ALL SELECT generation FROM memory_projection_generations WHERE scope_type='global' AND scope_value='_global') heads),0))::bigint::text
 FROM memory_collection_owner o WHERE o.id=1)`

type projectedRule struct {
	ID            int64  `json:"id"`
	Polarity      string `json:"polarity"`
	Title         string `json:"title"`
	Description   string `json:"description"`
	Weight        int    `json:"weight"`
	Domain        string `json:"domain"`
	CreatedAt     string `json:"created_at"`
	UpdatedAt     string `json:"updated_at"`
	DirectiveType string `json:"directive_type"`
	ExpiresAt     string `json:"expires_at"`
	Tier          string `json:"tier"`
}

func (s *postgresDataStore) currentRules(ctx context.Context, after int64, limit int, weighted bool) ([]projectedRule, error) {
	order := "r.id"
	if weighted {
		order = "r.weight DESC,r.id"
	}
	rows, err := s.db.Query(ctx, `SELECT r.id,r.polarity,r.title,r.description,r.weight,r.domain,
 r.created_at,r.updated_at,r.directive_type,COALESCE(r.expires_at,'') FROM rules r
 WHERE r.id>$1 AND `+memoryUnexpiredAtSQL("r.expires_at", "CURRENT_TIMESTAMP")+`
 AND `+currentRuleInputsSQL("r.")+` ORDER BY `+order+` LIMIT $2`, after, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := []projectedRule{}
	for rows.Next() {
		var r projectedRule
		if err := rows.Scan(&r.ID, &r.Polarity, &r.Title, &r.Description, &r.Weight, &r.Domain, &r.CreatedAt, &r.UpdatedAt, &r.DirectiveType, &r.ExpiresAt); err != nil {
			return nil, err
		}
		r.Tier = "Archived"
		if r.Weight >= 75 {
			r.Tier = "Rule"
		} else if r.Weight >= 50 {
			r.Tier = "Inclination"
		}
		result = append(result, r)
	}
	return result, rows.Err()
}

// Preserve the native format and its 20-rule/2000-byte limits. Complete UTF-8
// characters are retained when the 120-byte per-rule budget requires truncation.
func rulesMarkdown(rules []projectedRule) string {
	var b strings.Builder
	b.WriteString("# Rules\n\n")
	emitted := 0
	for _, bounds := range [][2]int{{75, 100}, {50, 74}, {0, 49}} {
		for _, r := range rules {
			if emitted == 20 {
				return b.String()
			}
			if r.Weight < bounds[0] || r.Weight > bounds[1] {
				continue
			}
			text := r.Description
			if text == "" {
				text = r.Title
			}
			if len(text) > 120 {
				n := 120
				for n > 0 && !utf8.RuneStart(text[n]) {
					n--
				}
				text = text[:n] + "..."
			}
			symbol := "~"
			if r.Polarity == "positive" {
				symbol = "+"
			} else if r.Polarity == "negative" {
				symbol = "-"
			}
			prefix := ""
			if r.DirectiveType == "hard" {
				prefix = "MUST: "
			} else if r.DirectiveType == "soft" {
				prefix = "SHOULD: "
			}
			line := fmt.Sprintf("- (%s %d) %s%s\n", symbol, r.Weight, prefix, text)
			if b.Len()+len(line) >= 2000 {
				break
			}
			b.WriteString(line)
			emitted++
		}
	}
	return b.String()
}

func (s *postgresDataStore) exportCurrentRules(ctx context.Context, path string) (int, error) {
	f, err := os.CreateTemp(filepath.Dir(path), ".aimee-rules-export-*")
	if err != nil {
		return 0, err
	}
	defer func() { f.Close(); os.Remove(f.Name()) }()
	encoder := json.NewEncoder(f)
	after, count := int64(0), 0
	for {
		if err := ctx.Err(); err != nil {
			return 0, err
		}
		rules, err := s.currentRules(ctx, after, 128, false)
		if err != nil {
			return 0, err
		}
		if len(rules) == 0 {
			break
		}
		for _, r := range rules {
			if err := encoder.Encode(map[string]any{"polarity": r.Polarity, "title": r.Title, "description": r.Description, "weight": r.Weight, "domain": r.Domain, "created_at": r.CreatedAt, "updated_at": r.UpdatedAt}); err != nil {
				return 0, err
			}
			after = r.ID
			count++
		}
	}
	if err := f.Sync(); err != nil {
		return 0, err
	}
	if err := f.Close(); err != nil {
		return 0, err
	}
	if err := ctx.Err(); err != nil {
		return 0, err
	}
	return count, os.Rename(f.Name(), path)
}

func (s *postgresDataStore) ruleView(ctx context.Context, request DataRequest) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	if request.Kind == "export" {
		if request.Path == "" {
			return nil, fmt.Errorf("memory: rules export requires a path")
		}
		n, err := s.exportCurrentRules(ctx, request.Path)
		if err != nil {
			return nil, err
		}
		return json.Marshal(map[string]any{"status": "ok", "count": n})
	}
	limit := request.Limit
	if request.Kind == "generate" {
		limit = 128
	}
	rules, err := s.currentRules(ctx, 0, limit, true)
	if err != nil {
		return nil, err
	}
	if request.Kind == "generate" {
		return json.Marshal(map[string]any{"status": "ok", "content": rulesMarkdown(rules)})
	}
	if request.Kind != "list" {
		return nil, fmt.Errorf("memory: unsupported rule view")
	}
	return json.Marshal(map[string]any{"status": "ok", "rules": rules})
}

func handleRuleView(options handlerOptions, invocation bus.ModuleInvocation, args commandArgs) ([]byte, bus.ModuleStatus) {
	if options.placement != PlacementKB {
		return nil, bus.ModuleStatusCapabilityAbsent
	}
	kind := strings.TrimPrefix(args.stringOr("operation", ""), "rules-")
	request := DataRequest{Operation: "rules-view", Kind: kind, Scope: Scope{Type: ScopeGlobal, Value: "_global"}, Limit: args.limit("limit", 128, 1024), Path: args.stringOr("path", "")}
	if kind != "list" && kind != "generate" && kind != "export" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if kind == "export" && request.Path == "" {
		return runtimeJSONText(commandResult(commandError("invalid_argument", "rules export requires a path")))
	}
	body, _ := json.Marshal(request)
	raw, status := handleData(options, invocation, body)
	if status != bus.ModuleStatusOK {
		return nil, status
	}
	var response DataResponse
	if json.Unmarshal(raw, &response) != nil || len(response.Payload) == 0 {
		return nil, bus.ModuleStatusInternal
	}
	return runtimeJSONText(commandResult(response.Payload))
}
