package db2

import (
	"context"
	"fmt"
)

// The expiry, demotion, promotion and health-snapshot statements, ported from
// memory_promotion.c and memory_health.c.
const (
	sqlDeleteL0Provenance = `DELETE FROM memory_provenance WHERE memory_id IN` +
		` (SELECT id FROM memories WHERE tier = 'L0')`

	sqlDeleteL0 = `DELETE FROM memories WHERE tier = 'L0'`

	sqlListKindsInTier = `SELECT DISTINCT kind FROM memories WHERE tier = $1`

	// The stale-L1 predicate reads its window from the row when the row
	// overrides it, and from the argument otherwise. The argument arrives
	// signed ("-30") and has the sign stripped here because COALESCE needs both
	// arms to be a day count, not a signed offset.
	sqlDeleteStaleL1Provenance = `DELETE FROM memory_provenance WHERE memory_id IN` +
		` (SELECT id FROM memories WHERE tier = 'L1'` +
		`   AND epistemic_kind = $1` +
		`   AND last_used_at < pg_now_text('-' || COALESCE(` +
		`     expiry_days_migration_override,CAST(replace($2,'-','') AS bigint))` +
		`     || ' days'))`

	sqlDeleteStaleL1 = `DELETE FROM memories WHERE tier = 'L1' AND epistemic_kind = $1` +
		`  AND last_used_at < pg_now_text('-' || COALESCE(` +
		`    expiry_days_migration_override,CAST(replace($2,'-','') AS bigint))` +
		`    || ' days')`

	sqlDemoteKind = `UPDATE memories SET tier = 'L1', updated_at = $1` +
		` WHERE tier = 'L2' AND kind = $2` +
		`   AND confidence < $3` +
		`   AND last_used_at < pg_now_text($4 || ' days')`

	// The cascade weakens rows that depend on what this stamp just demoted. It
	// keys on the stamp rather than a row list because the demotion may have
	// touched more rows than any caller wants to carry.
	sqlDemoteCascade = `UPDATE memories SET confidence = confidence * 0.9` +
		` WHERE id IN (` +
		`  SELECT ml.source_id FROM memory_links ml` +
		`  JOIN memories m ON m.id = ml.target_id` +
		`  WHERE ml.relation = 'depends_on'` +
		`    AND m.tier = 'L1' AND m.updated_at = $1` +
		`)`

	sqlPromoteStable = `UPDATE memories SET tier = 'L3', updated_at = $1` +
		` WHERE tier = 'L2'` +
		`   AND kind IN ('fact', 'preference')` +
		`   AND confidence >= 0.95` +
		`   AND use_count >= 5` +
		`   AND updated_at <= pg_now_text('-30 days')`

	sqlReclassifyDirectivesOpen = `UPDATE memories SET tier = 'L4'` +
		` WHERE tier = 'L3'` +
		`   AND (kind='workflow' OR (kind='policy' AND` +
		` (epistemic_kind<>'policy' OR governance_promoted<>0)))`

	sqlReclassifyDirectivesApproved = `UPDATE memories SET tier = 'L4'` +
		` WHERE tier = 'L3'` +
		`   AND (kind = 'workflow'` +
		`        OR (kind = 'policy' AND (epistemic_kind<>'policy' OR` +
		` governance_promoted<>0) AND id IN` +
		`            (SELECT memory_id FROM memory_promotion_approvals` +
		`             WHERE target_tier = 'L4')))`

	sqlRecordL4Approval = `INSERT INTO memory_promotion_approvals` +
		`  (memory_id, target_tier, approver, note, approved_at)` +
		` VALUES ($1, 'L4', $2, $3, pg_now_text())` +
		` ON CONFLICT (memory_id, target_tier) DO UPDATE SET` +
		`   approver = excluded.approver,` +
		`   note = excluded.note,` +
		`   approved_at = excluded.approved_at`

	sqlCountMemories = `SELECT COUNT(*) FROM memories`

	sqlCountRecentConflicts = `SELECT COUNT(*) FROM memory_conflicts` +
		` WHERE detected_at >= pg_now_text($1)`

	sqlHealthRecord = `INSERT INTO memory_health` +
		` (cycle_at, total_memories, contradictions_detected,` +
		`  promotions, demotions, expirations)` +
		` VALUES ($1, $2, $3, $4, $5, $6)`

	sqlPruneHealth = `DELETE FROM memory_health WHERE cycle_at < pg_now_text($1)`

	sqlPruneContradictions = `DELETE FROM contradiction_log` +
		` WHERE detected_at < pg_now_text($1)`
)

// defaultApprover is the attribution used when a caller records an approval
// without naming one, matching the C implementation. An unattributed approval
// is still a real approval and must not be recorded as an empty string.
const defaultApprover = "operator"

// retentionWindow renders the "-90 days" spelling the retention and conflict
// statements pass straight to pg_now_text. Distinct from sweepWindow, whose
// statements append the unit themselves.
func retentionWindow(days uint32) string {
	return fmt.Sprintf("-%d days", days)
}

func (b *pgMemoryBackend) DeleteL0Provenance(ctx context.Context) (uint32, error) {
	var affected uint32
	err := b.execCounted(ctx, sqlDeleteL0Provenance, &affected)
	return affected, err
}

func (b *pgMemoryBackend) DeleteL0(ctx context.Context) (uint32, error) {
	var affected uint32
	err := b.execCounted(ctx, sqlDeleteL0, &affected)
	return affected, err
}

func (b *pgMemoryBackend) ListKindsInTier(ctx context.Context, tier string, max uint32) ([]string, error) {
	if b == nil || b.query == nil {
		return nil, ErrNoQuerier
	}
	if tier == "" || max == 0 {
		return nil, nil
	}
	rows, err := b.query(ctx, sqlListKindsInTier, tier)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	kinds := make([]string, 0, 8)
	for rows.Next() {
		var kind *string
		if err := rows.Scan(&kind); err != nil {
			return nil, err
		}
		// A NULL kind names no policy, so there is no window to sweep it by.
		// The C loop skips it for the same reason.
		if kind == nil || *kind == "" {
			continue
		}
		kinds = append(kinds, *kind)
		if uint32(len(kinds)) >= max {
			break
		}
	}
	return kinds, rows.Err()
}

func (b *pgMemoryBackend) DeleteStaleL1Provenance(ctx context.Context, kind, window string) (uint32, error) {
	if kind == "" || window == "" {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlDeleteStaleL1Provenance, &affected, kind, window)
	return affected, err
}

func (b *pgMemoryBackend) DeleteStaleL1(ctx context.Context, kind, window string) (uint32, error) {
	if kind == "" || window == "" {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlDeleteStaleL1, &affected, kind, window)
	return affected, err
}

func (b *pgMemoryBackend) DemoteKind(ctx context.Context, stamp, kind string, confidence float64, window string) (uint32, error) {
	if stamp == "" || kind == "" || window == "" {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlDemoteKind, &affected, stamp, kind, confidence, window)
	return affected, err
}

func (b *pgMemoryBackend) DemoteCascade(ctx context.Context, stamp string) (uint32, error) {
	if stamp == "" {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlDemoteCascade, &affected, stamp)
	return affected, err
}

func (b *pgMemoryBackend) PromoteStable(ctx context.Context, stamp string) (uint32, error) {
	if stamp == "" {
		return 0, nil
	}
	var affected uint32
	err := b.execCounted(ctx, sqlPromoteStable, &affected, stamp)
	return affected, err
}

func (b *pgMemoryBackend) ReclassifyDirectives(ctx context.Context, requireApproval bool) (uint32, error) {
	query := sqlReclassifyDirectivesOpen
	if requireApproval {
		query = sqlReclassifyDirectivesApproved
	}
	var affected uint32
	err := b.execCounted(ctx, query, &affected)
	return affected, err
}

func (b *pgMemoryBackend) RecordL4Approval(ctx context.Context, memoryID uint64, approver, note string) error {
	if memoryID == 0 {
		return fmt.Errorf("db2: an L4 approval needs a memory id")
	}
	if approver == "" {
		approver = defaultApprover
	}
	return b.execCounted(ctx, sqlRecordL4Approval, nil, int64(memoryID), approver, note)
}

func (b *pgMemoryBackend) CountMemories(ctx context.Context) (uint32, error) {
	return b.count(ctx, sqlCountMemories)
}

// CountRecentConflicts floors the window at one day.
//
// A zero or absent window would render "-0 days", which is "now" — reporting no
// recent conflicts on a system that may have plenty. The C implementation
// applies the same floor.
func (b *pgMemoryBackend) CountRecentConflicts(ctx context.Context, days uint32) (uint32, error) {
	if days == 0 {
		days = 1
	}
	return b.count(ctx, sqlCountRecentConflicts, retentionWindow(days))
}

func (b *pgMemoryBackend) HealthRecord(ctx context.Context, snapshot HealthSnapshot) error {
	return b.execCounted(ctx, sqlHealthRecord, nil,
		nowUTC(b.now()),
		int64(snapshot.TotalMemories),
		int64(snapshot.ContradictionsDetected),
		int64(snapshot.Promotions),
		int64(snapshot.Demotions),
		int64(snapshot.Expirations))
}

// PruneHealth and PruneContradictions default their window to ninety days
// rather than treating zero as "prune everything", which is what an unfloored
// window would mean.
func (b *pgMemoryBackend) PruneHealth(ctx context.Context, days uint32) (uint32, error) {
	if days == 0 {
		days = 90
	}
	var affected uint32
	err := b.execCounted(ctx, sqlPruneHealth, &affected, retentionWindow(days))
	return affected, err
}

func (b *pgMemoryBackend) PruneContradictions(ctx context.Context, days uint32) (uint32, error) {
	if days == 0 {
		days = 90
	}
	var affected uint32
	err := b.execCounted(ctx, sqlPruneContradictions, &affected, retentionWindow(days))
	return affected, err
}
