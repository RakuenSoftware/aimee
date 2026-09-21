package memory

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
)

// Match persisted native entity keys, including the legacy bounded hash input.
// Changing this encoding would disconnect existing curator edges.
func hybridSymbolKey(project, symbol string) string {
	encode := func(s string) string {
		out := make([]byte, 0, 511)
		const digits = "0123456789ABCDEF"
		for i := 0; i < len(s) && len(out) < 511; i++ {
			c := s[i]
			if c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z' || c >= '0' && c <= '9' || c == '.' || c == '_' || c == '-' || c == '~' || c == '/' {
				out = append(out, c)
			} else {
				if len(out)+3 >= 512 {
					break
				}
				out = append(out, '%', digits[c>>4], digits[c&15])
			}
		}
		return string(out)
	}
	full := "symbol:" + encode(project) + ":" + encode(symbol)
	if len(full) <= 511 {
		return full
	}
	if len(full) > 1023 {
		full = full[:1023]
	}
	sum := sha256.Sum256([]byte(full))
	return "symbol:h:" + hex.EncodeToString(sum[:16])
}

type hybridMemoryFile struct {
	Project          string  `json:"project"`
	Path             string  `json:"file_path"`
	StructuralWeight float64 `json:"structural_weight"`
}
type hybridMemoryWhy struct {
	ID       int64  `json:"id"`
	Kind     string `json:"kind"`
	Headline string `json:"headline,omitempty"`
	Content  string `json:"content"`
}
type hybridMemoryResult struct {
	Status string             `json:"status"`
	Files  []hybridMemoryFile `json:"files"`
	// The native host embeds these trusted JSON bytes without converting int64
	// IDs through cJSON's double representation or truncating memory content.
	WhyJSON string `json:"why_json"`
}

func (s *postgresDataStore) hybridContext(ctx context.Context, request DataRequest) (hybridMemoryResult, error) {
	result := hybridMemoryResult{Status: "ok", Files: []hybridMemoryFile{}}
	request.Limit = 5
	var records []Record
	var err error
	if request.Project != "" || !request.IncludeAll {
		records, err = s.SearchVisible(ctx, request)
	} else {
		records, err = s.QueryRecords(ctx, "like", request.Query, 0, 5)
	}
	if err != nil {
		return result, err
	}
	public, err := s.publicRecords(ctx, records)
	if err != nil {
		return result, err
	}
	why := make([]hybridMemoryWhy, 0, len(public))
	for _, r := range public {
		why = append(why, hybridMemoryWhy{r.ID, r.Kind, r.Headline, r.Content})
	}
	encoded, err := json.Marshal(why)
	if err != nil {
		return result, err
	}
	result.WhyJSON = string(encoded)
	if !s.graphFusionEnabled() || request.Project == "" || request.Entity == "" {
		return result, nil
	}
	// Scope, evidence, and generation checks precede both deduplication and LIMIT.
	// A private/stale neighbor cannot hide an eligible file or borrow its project.
	rows, err := s.db.Query(ctx, `WITH candidates AS (
 SELECT n.project,n.file_path,e.structural_weight,e.weight,e.id,
 row_number() OVER (PARTITION BY n.project,n.file_path ORDER BY (e.structural_weight+e.weight) DESC,e.id) AS rank
 FROM entity_edges e JOIN entity_nodes n ON n.node_key=CASE WHEN e.source=$1 THEN e.target ELSE e.source END
 WHERE (e.source=$1 OR e.target=$1) AND n.node_key<>$1 AND e.edge_class<>'semantic'
 AND n.project=$2 AND n.file_path<>''
 AND (e.edge_origin<>'code_projection' OR EXISTS(SELECT 1 FROM code_projection_generations g
 JOIN projects p ON p.name=g.project WHERE g.id=e.projection_generation_id AND g.state='visible'
 AND g.project=$2 AND p.lifecycle_state='current'))
 AND (n.node_origin<>'code_projection' OR EXISTS(SELECT 1 FROM code_projection_generations g
 JOIN projects p ON p.name=g.project WHERE g.id=n.last_seen_generation_id AND g.state='visible'
 AND g.project=$2 AND p.lifecycle_state='current'))
 AND `+currentMemoryEvidenceSQL("e", `$3 OR m.scope_type='global' OR (m.scope_type='workspace' AND m.scope_value='_shared') OR (m.scope_type='project' AND m.scope_value=$2)
 OR (m.scope_type='workspace' AND m.scope_value=$4)`, false)+`
 ) SELECT project,file_path,structural_weight FROM candidates WHERE rank=1
 ORDER BY (structural_weight+weight) DESC,id LIMIT 25`, hybridSymbolKey(request.Project, request.Entity), request.Project, request.IncludeAll, request.Workspace)
	if err != nil {
		return result, err
	}
	defer rows.Close()
	for rows.Next() {
		var file hybridMemoryFile
		if err = rows.Scan(&file.Project, &file.Path, &file.StructuralWeight); err != nil {
			return result, err
		}
		result.Files = append(result.Files, file)
	}
	return result, rows.Err()
}
