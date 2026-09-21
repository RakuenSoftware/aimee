package memory

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"time"
)

func parseMemoryTime(value string) (time.Time, error) {
	for _, layout := range []string{time.RFC3339Nano, "2006-01-02 15:04:05.999999999Z07:00",
		"2006-01-02 15:04:05.999999999", "2006-01-02 15:04:05", "2006-01-02"} {
		if parsed, err := time.Parse(layout, value); err == nil {
			return parsed, nil
		}
	}
	return time.Time{}, fmt.Errorf("memory: invalid timestamp %q", value)
}

func (s *postgresDataStore) ValidAt(ctx context.Context, id int64, asOf string) (bool, error) {
	if s.placement != PlacementKB {
		return false, errors.New("memory: temporal memory belongs to kb placement")
	}
	when, err := parseMemoryTime(asOf)
	if err != nil {
		return false, err
	}
	var fromText, untilText string
	err = s.db.QueryRow(ctx, `SELECT COALESCE(valid_from, ''), COALESCE(valid_until, '')
FROM memories WHERE id = $1`, id).Scan(&fromText, &untilText)
	if err != nil {
		return false, err
	}
	if strings.TrimSpace(fromText) != "" {
		from, parseErr := parseMemoryTime(strings.TrimSpace(fromText))
		if parseErr != nil {
			return false, parseErr
		}
		if when.Before(from) {
			return false, nil
		}
	}
	if strings.TrimSpace(untilText) != "" {
		until, parseErr := parseMemoryTime(strings.TrimSpace(untilText))
		if parseErr != nil {
			return false, parseErr
		}
		if !when.Before(until) {
			return false, nil
		}
	}
	return true, nil
}

const (
	factRecallMaxFacts    = 32
	factRecallMaxEntities = 8
	factRecallLineCap     = 256
)

// recallFactBlock owns typed-fact selection, ordering, formatting, and PII
// policy. C callers receive the finished block over the event bus and do not
// inspect the database or make memory decisions.
func (s *postgresDataStore) recallFactBlock(ctx context.Context, entity string,
	turnRequestsSensitive bool, capacity int) (string, int, error) {
	rows, err := s.db.Query(ctx, `SELECT relation, target, confidence FROM entity_edges
WHERE source = $1 AND edge_class = 'semantic'
  AND lifecycle_state IN ('persistent','promoted')
  AND superseded_at = '' AND invalidated_at = '' AND suppressed = 0
ORDER BY confidence DESC, id ASC LIMIT $2`, entity, factRecallMaxFacts)
	if err != nil {
		return "", 0, err
	}
	defer rows.Close()

	var block strings.Builder
	count := 0
	for rows.Next() {
		var relation, target string
		var confidence float64
		if err := rows.Scan(&relation, &target, &confidence); err != nil {
			return "", 0, err
		}
		if relation == "" || target == "" ||
			!ShouldInject(RelSensitivityOf(relation), confidence, turnRequestsSensitive) {
			continue
		}
		line := fmt.Sprintf("- %s: %s\n", relation, target)
		if len(line) >= factRecallLineCap {
			continue
		}
		// The legacy ABI capacity includes the trailing NUL. Preserve that
		// contract so the bus adapter can copy the returned block verbatim.
		if block.Len()+len(line) >= capacity {
			break
		}
		block.WriteString(line)
		count++
	}
	if err := rows.Err(); err != nil {
		return "", 0, err
	}
	return block.String(), count, nil
}

func (s *postgresDataStore) mentionedEntities(ctx context.Context, query string) []string {
	rows, err := s.db.Query(ctx, `SELECT (
  SELECT name FROM entity_aliases p WHERE p.canonical_id = r.canonical_id
    AND p.suppressed = 0 ORDER BY is_preferred DESC, id ASC LIMIT 1
) AS pref
FROM entity_registry r
WHERE r.status = 'active'
  AND EXISTS (SELECT 1 FROM entity_aliases a WHERE a.canonical_id = r.canonical_id
    AND a.suppressed = 0 AND length(a.name_norm) >= 3
    AND lower($1) LIKE '%' || a.name_norm || '%')
LIMIT $2`, query, factRecallMaxEntities)
	if err != nil {
		return nil
	}
	names := make([]string, 0, factRecallMaxEntities)
	seen := make(map[string]struct{}, factRecallMaxEntities)
	for rows.Next() && len(names) < factRecallMaxEntities {
		var name string
		if scanErr := rows.Scan(&name); scanErr != nil {
			break
		}
		if name != "" && name != "user" {
			if _, exists := seen[name]; !exists {
				seen[name] = struct{}{}
				names = append(names, name)
			}
		}
	}
	rows.Close()

	if len(names) >= factRecallMaxEntities {
		return names
	}
	rows, err = s.db.Query(ctx, `SELECT DISTINCT source FROM entity_edges
WHERE edge_class = 'semantic' AND source <> 'user' AND length(source) >= 3
  AND lifecycle_state IN ('persistent','promoted')
  AND superseded_at = '' AND invalidated_at = '' AND suppressed = 0
  AND lower($1) LIKE '%' || lower(source) || '%'
ORDER BY source LIMIT $2`, query, factRecallMaxEntities)
	if err != nil {
		return names
	}
	defer rows.Close()
	for rows.Next() && len(names) < factRecallMaxEntities {
		var name string
		if scanErr := rows.Scan(&name); scanErr != nil {
			break
		}
		if name == "" {
			continue
		}
		if _, exists := seen[name]; exists {
			continue
		}
		seen[name] = struct{}{}
		names = append(names, name)
	}
	return names
}

func (s *postgresDataStore) RecallFacts(ctx context.Context, entity, query string,
	turnRequestsSensitive bool, capacity int) (string, int, error) {
	if s.placement != PlacementKB {
		return "", 0, errors.New("memory: typed fact recall belongs to kb placement")
	}
	if capacity < 1 {
		return "", 0, errors.New("memory: typed fact recall requires output capacity")
	}
	if entity != "" {
		return s.recallFactBlock(ctx, entity, turnRequestsSensitive, capacity)
	}

	block, total, err := s.recallFactBlock(ctx, "user", turnRequestsSensitive, capacity)
	if err != nil {
		return "", 0, err
	}
	for _, name := range s.mentionedEntities(ctx, query) {
		remaining := capacity - len(block)
		if remaining <= 1 {
			break
		}
		addition, count, recallErr := s.recallFactBlock(ctx, name, turnRequestsSensitive, remaining)
		if recallErr != nil {
			continue
		}
		block += addition
		total += count
	}
	return block, total, nil
}
