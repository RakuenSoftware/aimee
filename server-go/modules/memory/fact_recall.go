package memory

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"strconv"
	"strings"
	"time"
)

func parseMemoryTime(value string) (time.Time, error) {
	for _, layout := range []string{time.RFC3339Nano, "2006-01-02 15:04:05.999999999Z07:00",
		"2006-01-02 15:04:05.999999999Z07", "2006-01-02 15:04:05.999999999", "2006-01-02 15:04:05", "2006-01-02"} {
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

// Serving typed facts requires every memory source to remain current and
// visible. Review/history queries retain their separate operator semantics.
var currentFactRecallSQL = `e.edge_class='semantic' AND e.lifecycle_state IN ('persistent','promoted')
 AND e.suppressed=0 AND ` + assertionCurrent + `
 AND ` + currentMemoryEvidenceSQL("e", "", false)

// recallFactBlockSources owns typed-fact selection, ordering, formatting, and PII
// policy. C callers receive the finished block over the event bus and do not
// inspect the database or make memory decisions.
func (s *postgresDataStore) recallFactBlockSources(ctx context.Context, entity string,
	turnRequestsSensitive bool, capacity int, refs *[]typedProjectionRef) (string, int, error) {
	columns := "relation,target,confidence"
	if refs != nil {
		columns += ",e.id,e.version,(SELECT owner_id::text FROM memory_collection_owner WHERE id=1)," + assertionMemoryVersions
	}
	rows, err := s.db.Query(ctx, `SELECT `+columns+` FROM entity_edges e
WHERE source = $1 AND `+currentFactRecallSQL+`
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
		var hit assertionHit
		var parents string
		columns := []any{&relation, &target, &confidence}
		if refs != nil {
			columns = append(columns, &hit.ID, &hit.Version, &hit.ownerID, &parents)
		}
		if err := rows.Scan(columns...); err != nil {
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
		if refs != nil {
			if err := json.Unmarshal([]byte(parents), &hit.memoryParents); err != nil {
				return "", 0, err
			}
			hit.StableID, hit.memoryParentsObserved = strconv.FormatInt(hit.ID, 10), true
			for i := range hit.memoryParents {
				hit.memoryParents[i].SchemaVersion, hit.memoryParents[i].OwnerID = 1, hit.ownerID
			}
			ref := typedProjectionRef{Channel: "facts", ID: hit.StableID, Source: hit.sourceVersion()}
			if ref.Source == nil || !validTypedSource(ref) {
				return "", 0, errors.New("memory: fact source versions unavailable or exceed capacity")
			}
			*refs = append(*refs, ref)
		}
		block.WriteString(line)
		count++
	}
	if err := rows.Err(); err != nil {
		return "", 0, err
	}
	return block.String(), count, nil
}

func (s *postgresDataStore) mentionedEntities(ctx context.Context, query string) ([]string, error) {
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
		return nil, err
	}
	names := make([]string, 0, factRecallMaxEntities)
	seen := make(map[string]struct{}, factRecallMaxEntities)
	for rows.Next() && len(names) < factRecallMaxEntities {
		var name string
		if scanErr := rows.Scan(&name); scanErr != nil {
			rows.Close()
			return nil, scanErr
		}
		if name != "" && name != "user" {
			if _, exists := seen[name]; !exists {
				seen[name] = struct{}{}
				names = append(names, name)
			}
		}
	}
	err = rows.Err()
	rows.Close()
	if err != nil {
		return nil, err
	}

	if len(names) >= factRecallMaxEntities {
		return names, nil
	}
	rows, err = s.db.Query(ctx, `SELECT DISTINCT source FROM entity_edges e
WHERE source <> 'user' AND length(source) >= 3 AND `+currentFactRecallSQL+`
  AND lower($1) LIKE '%' || lower(source) || '%'
ORDER BY source LIMIT $2`, query, factRecallMaxEntities)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	for rows.Next() && len(names) < factRecallMaxEntities {
		var name string
		if scanErr := rows.Scan(&name); scanErr != nil {
			rows.Close()
			return nil, scanErr
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
	return names, rows.Err()
}

func (s *postgresDataStore) RecallFacts(ctx context.Context, entity, query string,
	turnRequestsSensitive bool, capacity int) (string, int, error) {
	return s.recallFactsSources(ctx, entity, query, turnRequestsSensitive, capacity, nil)
}

func (s *postgresDataStore) recallFactsSources(ctx context.Context, entity, query string,
	turnRequestsSensitive bool, capacity int, refs *[]typedProjectionRef) (string, int, error) {
	if s.placement != PlacementKB {
		return "", 0, errors.New("memory: typed fact recall belongs to kb placement")
	}
	if capacity < 1 {
		return "", 0, errors.New("memory: typed fact recall requires output capacity")
	}
	if entity != "" {
		return s.recallFactBlockSources(ctx, entity, turnRequestsSensitive, capacity, refs)
	}
	// Query recall owns classification as well as filtering. Native callers no
	// longer classify the turn separately, and a caller-supplied true flag cannot
	// turn an unrelated query into permission to include PII.
	turnRequestsSensitive = TurnRequestsSensitive(query)

	block, total, err := s.recallFactBlockSources(ctx, "user", turnRequestsSensitive, capacity, refs)
	if err != nil {
		return "", 0, err
	}
	names, err := s.mentionedEntities(ctx, query)
	if err != nil {
		return "", 0, err
	}
	for _, name := range names {
		remaining := capacity - len(block)
		if remaining <= 1 {
			break
		}
		addition, count, recallErr := s.recallFactBlockSources(ctx, name, turnRequestsSensitive, remaining, refs)
		if recallErr != nil {
			return "", 0, recallErr
		}
		block += addition
		total += count
	}
	return block, total, nil
}
