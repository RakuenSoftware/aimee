package memory

import (
	"context"
	"encoding/json"
	"time"
)

const (
	MaintenanceReplay    uint32 = 1 << 0
	MaintenanceCompact   uint32 = 1 << 1
	MaintenancePrune     uint32 = 1 << 2
	MaintenanceSummarize uint32 = 1 << 3
	MaintenanceDrift     uint32 = 1 << 4
	MaintenanceDefault          = MaintenanceReplay | MaintenanceCompact | MaintenancePrune
)

type EffectivenessStats struct {
	Average         float64 `json:"average"`
	LowCount        int     `json:"low_count"`
	HighImpactCount int     `json:"high_impact_count"`
	NeverSurfacedL2 int     `json:"never_surfaced_l2"`
}

type LintIssue struct {
	Type     string `json:"type"`
	MemoryID int64  `json:"memory_id,omitempty"`
	Key      string `json:"key,omitempty"`
	Message  string `json:"message"`
}

type MaintenanceSummary struct {
	ModesRun              uint32  `json:"modes_run"`
	Skipped               bool    `json:"skipped"`
	DryRun                bool    `json:"dry_run"`
	Promoted              int     `json:"promoted"`
	Demoted               int     `json:"demoted"`
	Expired               int     `json:"expired"`
	LifecycleArchived     int     `json:"lifecycle_archived"`
	RemindersExpired      int     `json:"reminders_expired"`
	DirectivesExpired     int     `json:"directives_expired"`
	Rescored              int     `json:"rescored"`
	ProfileCardsRefreshed int     `json:"profile_cards_refreshed"`
	Merged                int     `json:"merged"`
	Summarized            int     `json:"summarized"`
	DriftCandidates       int     `json:"drift_candidates"`
	DriftRequeued         int     `json:"drift_requeued"`
	ElapsedMS             float64 `json:"elapsed_ms"`
	MemoryCountBefore     int64   `json:"memory_count_before"`
	MemoryCountAfter      int64   `json:"memory_count_after"`
}

func (s *postgresDataStore) EffectivenessStats(ctx context.Context) (EffectivenessStats, error) {
	if err := s.requireKBDomain(); err != nil {
		return EffectivenessStats{}, err
	}
	var out EffectivenessStats
	err := s.db.QueryRow(ctx, `SELECT COALESCE(AVG(effectiveness),0),
COUNT(*) FILTER (WHERE effectiveness<0.3),
COUNT(*) FILTER (WHERE effectiveness>=0.8 AND use_count>=3),
COUNT(*) FILTER (WHERE tier='L2' AND use_count=0)
FROM memories`).Scan(&out.Average, &out.LowCount, &out.HighImpactCount, &out.NeverSurfacedL2)
	return out, err
}

func (s *postgresDataStore) Lint(ctx context.Context, limit int) ([]LintIssue, error) {
	if err := s.requireKBDomain(); err != nil {
		return nil, err
	}
	rows, err := s.db.Query(ctx, `SELECT issue_type,id,key,message FROM (
 SELECT 'orphan' AS issue_type,m.id,m.key,'L0 memory has no provenance, links, or explicit scope' AS message
 FROM memories m WHERE m.tier='L0' AND NOT EXISTS(SELECT 1 FROM memory_provenance p WHERE p.memory_id=m.id)
 AND NOT EXISTS(SELECT 1 FROM memory_links l WHERE l.source_id=m.id OR l.target_id=m.id)
 AND NOT EXISTS(SELECT 1 FROM memory_scopes s WHERE s.memory_id=m.id)
 UNION ALL
 SELECT 'concept_gap',m.id,m.key,'memory has no extracted entity or relation' FROM memories m
 WHERE m.lifecycle_state='active' AND m.kind IN ('fact','pattern')
 AND NOT EXISTS(SELECT 1 FROM memory_entities e WHERE e.memory_id=m.id)
 AND NOT EXISTS(SELECT 1 FROM memory_relations r WHERE r.memory_id=m.id)
 UNION ALL
 SELECT 'stale_ref',m.id,m.key,'artifact reference has no content hash' FROM memories m
 WHERE COALESCE(m.artifact_ref,'')<>'' AND COALESCE(m.artifact_hash,'')=''
) issues ORDER BY id,issue_type LIMIT $1`, limit)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	issues := make([]LintIssue, 0)
	for rows.Next() {
		var issue LintIssue
		if err := rows.Scan(&issue.Type, &issue.MemoryID, &issue.Key, &issue.Message); err != nil {
			return nil, err
		}
		issues = append(issues, issue)
	}
	return issues, rows.Err()
}

func (s *postgresDataStore) countMemories(ctx context.Context) (int64, error) {
	var count int64
	err := s.db.QueryRow(ctx, `SELECT COUNT(*) FROM memories`).Scan(&count)
	return count, err
}

func (s *postgresDataStore) maintenanceChange(ctx context.Context, query string) (int, error) {
	tag, err := s.db.Exec(ctx, query)
	if err != nil {
		return 0, err
	}
	return int(tag.RowsAffected()), nil
}

func (s *postgresDataStore) RunMaintenance(ctx context.Context, modes uint32, force, dryRun bool) (MaintenanceSummary, error) {
	if err := s.requireKBDomain(); err != nil {
		return MaintenanceSummary{}, err
	}
	started := time.Now()
	if modes == 0 {
		modes = MaintenanceDefault
	}
	out := MaintenanceSummary{ModesRun: modes, DryRun: dryRun}
	if !force {
		var recent bool
		if err := s.db.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM kb_meta
WHERE key='memory_maintenance_last_run' AND value::timestamp>CURRENT_TIMESTAMP-interval '15 minutes')`).Scan(&recent); err != nil {
			return out, err
		}
		if recent {
			out.Skipped = true
			out.ElapsedMS = float64(time.Since(started).Microseconds()) / 1000
			return out, nil
		}
	}
	var err error
	out.MemoryCountBefore, err = s.countMemories(ctx)
	if err != nil {
		return out, err
	}
	if !dryRun && modes&MaintenanceReplay != 0 {
		if out.Promoted, err = s.maintenanceChange(ctx, `UPDATE memories SET tier='L3',updated_at=pg_now_text()
WHERE lifecycle_state='active' AND tier='L2' AND confidence>=0.95 AND use_count>=5`); err != nil {
			return out, err
		}
		if out.Demoted, err = s.maintenanceChange(ctx, `UPDATE memories SET tier='L1',updated_at=pg_now_text()
WHERE lifecycle_state='active' AND tier='L2' AND confidence<0.4`); err != nil {
			return out, err
		}
		if out.Expired, err = s.maintenanceChange(ctx, `UPDATE memories SET lifecycle_state='retired',
activation_suppressed=1,valid_until=pg_now_text(),updated_at=pg_now_text()
WHERE lifecycle_state='active' AND tier IN ('L0','L1') AND updated_at<pg_now_text('-90 days')`); err != nil {
			return out, err
		}
	}
	if !dryRun && modes&MaintenancePrune != 0 {
		if out.LifecycleArchived, err = s.LifecycleSweep(ctx); err != nil {
			return out, err
		}
		if out.RemindersExpired, err = s.ProspectiveSweep(ctx); err != nil {
			return out, err
		}
		if out.DirectivesExpired, err = s.DirectiveSweep(ctx); err != nil {
			return out, err
		}
	}
	out.MemoryCountAfter, err = s.countMemories(ctx)
	if err != nil {
		return out, err
	}
	out.ElapsedMS = float64(time.Since(started).Microseconds()) / 1000
	encoded, err := json.Marshal(out)
	if err != nil {
		return out, err
	}
	_, err = s.db.Exec(ctx, `INSERT INTO kb_meta(key,value) VALUES('memory_maintenance_last_run',pg_now_text()),
('memory_maintenance_last_summary',$1) ON CONFLICT(key) DO UPDATE SET value=EXCLUDED.value`, string(encoded))
	return out, err
}
