package db2

import (
	"context"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

func init() {
	Register(db2contract.StageEffectivenessUpdate,
		db2contract.OperationEffectivenessUpdate, effectivenessUpdate)
	Register(db2contract.StageRetentionEnforce,
		db2contract.OperationRetentionEnforce, retentionEnforce)
	Register(db2contract.StageEffectivenessDemote,
		db2contract.OperationEffectivenessDemote, effectivenessDemote)
	Register(db2contract.StageEffectivenessStats,
		db2contract.OperationEffectivenessStats, effectivenessStats)
	Register(db2contract.StageL2MemoryIDs,
		db2contract.OperationL2MemoryIDs, l2MemoryIDs)
}

// The policy the adapter holds rather than the caller: these three operations
// take no request fields, and the numbers they act on are the module's own.
//
// Reproducing them here rather than taking them from configuration is what
// keeps the two implementations answering the same question -- a threshold a
// deployment could move is a threshold the two sides could disagree about
// silently.
const (
	retentionRestrictedSensitivity = "restricted"
	retentionRestrictedDays        = 7
	retentionSensitiveSensitivity  = "sensitive"
	retentionSensitiveDays         = 90

	effectivenessDemoteThreshold = 0.3
	effectivenessStatsLow        = 0.3

	l2MemoryIDsMax = 2048
)

// effectiveness is nulled rather than zeroed when the caller has no value.
//
// Zero is a real effectiveness -- it means measured and useless -- and NULL
// means never measured. The demote sweep and the statistics both test for NOT
// NULL, so writing a zero here would demote a memory nobody has judged.
const effectivenessUpdateQuery = `UPDATE memories SET effectiveness = $1 WHERE id = $2`

func effectivenessUpdate(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	memoryID, hasValue, value, err :=
		db2contract.DecodeEffectivenessUpdateRequest(request)
	if err != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	if memoryID == 0 {
		// The C refuses a non-positive identifier before it reaches the
		// statement. The reply is a result domain rather than a flag, so the
		// refusal is invalid_state -- passing one here would encode
		// ResultNotFound, which this reply does not admit.
		return resultReply(db2contract.ResultInvalidState,
			db2contract.EncodeEffectivenessUpdateReply)
	}
	var effectiveness *float64
	if hasValue != 0 {
		effectiveness = &value
	}
	result := db2contract.ResultOK
	if _, execErr := store.Exec(ctx, effectivenessUpdateQuery, effectiveness,
		int64(memoryID)); execErr != nil {
		result = db2contract.ResultInvalidState
	}
	return resultReply(result, db2contract.EncodeEffectivenessUpdateReply)
}

// Both retention windows in one statement.
//
// The C runs one delete per sensitivity and adds the counts. One statement
// deletes the same rows and cannot report a total that never existed at any
// instant -- the C's two deletes are separated by however long the first takes,
// so its sum is of two different moments.
const retentionEnforceQuery = `DELETE FROM memories
 WHERE (sensitivity = $1 AND created_at < pg_now_text($2))
    OR (sensitivity = $3 AND created_at < pg_now_text($4))`

// retentionEnforce deletes what the retention policy no longer allows kept.
func retentionEnforce(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeRetentionEnforceRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	deleted, execErr := store.Exec(ctx, retentionEnforceQuery,
		retentionRestrictedSensitivity, retentionWindow(retentionRestrictedDays),
		retentionSensitiveSensitivity, retentionWindow(retentionSensitiveDays))
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(deleted, db2contract.EncodeRetentionEnforceReply)
}

// retentionWindow spells a day count the way pg_now_text takes it.
func retentionWindow(days int) string {
	return "-" + itoa(uint32(days)) + " days"
}

// A memory measured and found ineffective drops a tier.
//
// NOT NULL is the load-bearing half of the predicate: never-measured memories
// have no effectiveness to be below the threshold, and demoting them would
// punish a memory for not having been used yet.
const effectivenessDemoteQuery = `UPDATE memories
 SET tier = 'L1', updated_at = pg_now_text()
 WHERE tier = 'L2' AND effectiveness IS NOT NULL AND effectiveness < $1`

func effectivenessDemote(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEffectivenessDemoteRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	demoted, execErr := store.Exec(ctx, effectivenessDemoteQuery,
		effectivenessDemoteThreshold)
	if execErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return removedCount(demoted, db2contract.EncodeEffectivenessDemoteReply)
}

// The average is over the measured memories only, which is why the CASE is
// there: AVG already skips NULL, and the CASE says so in the statement rather
// than leaving a reader to remember it.
//
// A corpus with nothing measured yet has no average, and the reply has no way
// to say "no average" -- so it answers zero, which reads as "measured and
// useless". The count beside it is what distinguishes the two.
const effectivenessStatsQuery = `SELECT
 COALESCE(AVG(CASE WHEN effectiveness IS NOT NULL THEN effectiveness END), 0),
 COUNT(*) FILTER (WHERE effectiveness IS NOT NULL AND effectiveness < $1),
 COUNT(*) FILTER (WHERE effectiveness IS NOT NULL AND effectiveness > 0.8
                    AND use_count >= 10)
 FROM memories`

func effectivenessStats(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeEffectivenessStatsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	var stats db2contract.EffectivenessStats
	var low, highImpact int64
	if err := store.QueryRow(ctx, effectivenessStatsQuery, effectivenessStatsLow).
		Scan(&stats.AvgEffectiveness, &low, &highImpact); err != nil {
		return nil, bus.ModuleStatusInternal
	}
	stats.LowEffectivenessCount = uint32(max(low, 0))
	stats.HighImpactCount = uint32(max(highImpact, 0))
	reply, encodeErr := db2contract.EncodeEffectivenessStatsReply(stats)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// The identifiers of everything at L2, ordered and bounded.
//
// The C's read has no ORDER BY and stops at its array's width, so which 2048 a
// larger tier answered with was the plan's choice and changed between runs.
// Ordered by identifier, the same call twice answers the same list.
const l2MemoryIDsQuery = `SELECT id FROM memories WHERE tier = 'L2'
 ORDER BY id LIMIT $1`

func l2MemoryIDs(ctx context.Context, store Store, request []byte) (
	[]byte, bus.ModuleStatus,
) {
	if db2contract.DecodeL2MemoryIDsRequest(request) != nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	rows, err := store.Query(ctx, l2MemoryIDsQuery, l2MemoryIDsMax)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	defer rows.Close()
	identifiers := []uint64{}
	for rows.Next() {
		var id int64
		if scanErr := rows.Scan(&id); scanErr != nil {
			return nil, bus.ModuleStatusInternal
		}
		identifiers = append(identifiers, uint64(max(id, 0)))
	}
	if rows.Err() != nil {
		return nil, bus.ModuleStatusInternal
	}
	reply, encodeErr := db2contract.EncodeL2MemoryIDsReply(identifiers)
	if encodeErr != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}

// resultReply answers a reply whose only field is a result code.
//
// Distinct from acknowledgement, which answers a flag. The two look alike and
// are not: a flag's "yes" is one, and a result's "yes" is zero -- so passing a
// flag where a result belongs encodes ResultNotFound for every success.
func resultReply(result uint32, encode func(uint32) ([]byte, error)) (
	[]byte, bus.ModuleStatus,
) {
	reply, err := encode(result)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return reply, bus.ModuleStatusOK
}
