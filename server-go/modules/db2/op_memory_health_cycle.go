package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageHealthRecord,
		db2contract.OperationHealthRecord, healthRecord)
	Register(db2contract.StageHealthRetention,
		db2contract.OperationHealthRetention, healthRetention)
	Register(db2contract.StageHealthCounters,
		db2contract.OperationHealthCounters, healthCounters)
}

// The windows and thresholds the adapter supplies, as it supplies them.
const (
	healthConflictWindowDays      = 1
	healthRetentionSnapshotDays   = 90
	healthRetentionContradictions = 90
	healthPromoteUseCount         = 3
	healthPromoteConfidence       = 0.9
)

// One statement where the C makes three calls: count the memories, count the
// day's conflicts, then insert a row quoting both.
//
// The counts are subqueries of the insert, so the snapshot is of one instant.
// The C reads them from separate statements and writes them together, which
// makes a cycle row that never described the corpus at any single moment.
const healthRecordQuery = `INSERT INTO memory_health
 (cycle_at, total_memories, contradictions_detected, promotions, demotions,
  expirations)
 SELECT pg_now_text(),
   (SELECT COUNT(*) FROM memories),
   (SELECT COUNT(*) FROM memory_conflicts
     WHERE detected_at >= pg_now_text($1)),
   $2, $3, $4`

// healthRecord writes one row of the memory system's own vital signs.
//
// The reply carries nothing at all -- not even an acknowledgement -- which is
// the C's shape: the health cycle is fire-and-forget, and a caller that could
// act on a failed write would have to be the thing being measured.
func healthRecord(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	promotions, demotions, expirations, err :=
		db2contract.DecodeHealthRecordRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if _, execErr := store.Exec(ctx, healthRecordQuery,
		retentionWindow(healthConflictWindowDays), int64(promotions),
		int64(demotions), int64(expirations)); execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeHealthRecordReply()
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// Both prunes in one statement, each answering its own count.
//
// The C runs two deletes and reports two numbers; this runs two deletes inside
// one statement and reports the same two. Doing it as one keeps a caller from
// seeing the snapshots pruned and the contradictions not, which is the state
// the C leaves behind when the second delete fails.
const healthRetentionQuery = `WITH pruned_snapshots AS (
   DELETE FROM memory_health WHERE cycle_at < pg_now_text($1) RETURNING 1
 ), pruned_contradictions AS (
   DELETE FROM contradiction_log WHERE detected_at < pg_now_text($2) RETURNING 1
 )
 SELECT (SELECT COUNT(*) FROM pruned_snapshots),
        (SELECT COUNT(*) FROM pruned_contradictions)`

func healthRetention(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeHealthRetentionRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var snapshots, contradictions int64
	if err := store.QueryRow(ctx, healthRetentionQuery,
		retentionWindow(healthRetentionSnapshotDays),
		retentionWindow(healthRetentionContradictions)).
		Scan(&snapshots, &contradictions); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeHealthRetentionReply(
		uint32(max(snapshots, 0)), uint32(max(contradictions, 0)))
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The nine counters, from the four reads the C makes, in one statement.
//
// They describe one seven-day window and are read as a set, so reading them
// separately lets a promotion land between two of them and produce a set that
// never held: cycles counting a promotion the tier totals do not show yet.
//
// The staleness test treats an empty stamp as stale, which is the C's and is
// load-bearing: the column is NOT NULL with an empty default, so a memory that
// has never been used has an empty stamp rather than a null one, and testing
// only for NULL would call every never-used memory fresh.
const healthCountersQuery = `SELECT
 (SELECT COUNT(*) FROM memory_health WHERE cycle_at >= pg_now_text('-7 days')),
 (SELECT COALESCE(SUM(contradictions_detected), 0) FROM memory_health
   WHERE cycle_at >= pg_now_text('-7 days')),
 (SELECT COALESCE(SUM(promotions), 0) FROM memory_health
   WHERE cycle_at >= pg_now_text('-7 days')),
 (SELECT COALESCE(SUM(demotions), 0) FROM memory_health
   WHERE cycle_at >= pg_now_text('-7 days')),
 (SELECT COALESCE(SUM(expirations), 0) FROM memory_health
   WHERE cycle_at >= pg_now_text('-7 days')),
 (SELECT COUNT(*) FROM memories WHERE created_at >= pg_now_text('-7 days')),
 (SELECT COUNT(*) FROM memories WHERE tier = 'L1'
   AND (use_count >= $1 OR confidence >= $2)),
 (SELECT COUNT(*) FROM memories WHERE tier = 'L2'),
 (SELECT COUNT(*) FROM memories WHERE tier = 'L2'
   AND (last_used_at IS NULL OR last_used_at = ''
        OR last_used_at < pg_now_text('-30 days')))`

func healthCounters(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeHealthCountersRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var cycles, contradictions, promotions, demotions, expirations int64
	var newMemories, l1Eligible, l2Total, l2Stale int64
	if err := store.QueryRow(ctx, healthCountersQuery, healthPromoteUseCount,
		healthPromoteConfidence).Scan(&cycles, &contradictions, &promotions,
		&demotions, &expirations, &newMemories, &l1Eligible, &l2Total,
		&l2Stale); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeHealthCountersReply(
		db2contract.HealthCounters{
			Cycles:              uint32(max(cycles, 0)),
			TotalContradictions: uint32(max(contradictions, 0)),
			TotalPromotions:     uint32(max(promotions, 0)),
			TotalDemotions:      uint32(max(demotions, 0)),
			TotalExpirations:    uint32(max(expirations, 0)),
			NewMemories:         uint32(max(newMemories, 0)),
			L1Eligible:          uint32(max(l1Eligible, 0)),
			L2Total:             uint32(max(l2Total, 0)),
			L2Stale30Days:       uint32(max(l2Stale, 0)),
		})
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
