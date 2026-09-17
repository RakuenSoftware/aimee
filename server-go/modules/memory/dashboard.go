package memory

import (
	"context"
	"encoding/json"
	"fmt"
)

// DashboardStats composes the dashboard in the owner. Scope counts are one
// grouped query rather than a native round trip for every memory and conflict.
func (s *postgresDataStore) DashboardStats(ctx context.Context) (json.RawMessage, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	stats, err := s.Stats(ctx)
	if err != nil {
		return nil, err
	}
	tiers := make([]map[string]any, 0, 6)
	for i := 0; i < 6; i++ {
		tier := fmt.Sprintf("L%d", i)
		tiers = append(tiers, map[string]any{"tier": tier, "functional_name": functionalTierName(tier), "count": stats.TierCounts[tier]})
	}
	kinds := make([]map[string]any, 0, len(stats.TierKinds))
	for _, r := range stats.TierKinds {
		kinds = append(kinds, map[string]any{"tier": r.Tier, "functional_name": functionalTierName(r.Tier), "kind": r.Kind, "count": r.Count})
	}
	rows, err := s.db.Query(ctx, `WITH tags AS (
 SELECT id AS memory_id,scope_type FROM memories
 UNION ALL SELECT memory_id,scope_type FROM memory_scopes WHERE memory_id IN (SELECT id FROM memories)
), primary_scopes AS (
 SELECT memory_id,MAX(CASE scope_type WHEN 'project' THEN 3 WHEN 'workspace' THEN 2 WHEN 'global' THEN 1 ELSE 0 END) AS level
 FROM tags GROUP BY memory_id
), conflicts AS (
 SELECT memory_a AS memory_id FROM memory_conflicts WHERE resolved=0
 UNION ALL SELECT memory_b FROM memory_conflicts WHERE resolved=0
)
SELECT level,
 (SELECT count(*) FROM primary_scopes p JOIN memories m ON m.id=p.memory_id WHERE p.level=levels.level),
 (SELECT count(*) FROM conflicts c JOIN primary_scopes p ON p.memory_id=c.memory_id WHERE p.level=levels.level)
FROM generate_series(1,3) AS levels(level) ORDER BY level`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	scopes := make([]map[string]any, 0, 3)
	for rows.Next() {
		var level, count, conflicted int
		if err := rows.Scan(&level, &count, &conflicted); err != nil {
			return nil, err
		}
		scopes = append(scopes, map[string]any{"scope": scopeLevelName(level), "count": count, "conflicted_memories": conflicted})
	}
	if err := rows.Err(); err != nil {
		return nil, err
	}
	return json.Marshal(map[string]any{"tiers": tiers, "tier_kinds": kinds, "scopes": scopes})
}
