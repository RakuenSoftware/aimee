package db2

import (
	"context"
	"fmt"

	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// The two aggregate operations, ported from memory_health.c.
//
// Each is several statements rather than one join, matching the C source. The
// shape is deliberate: these read different tables over different windows, and
// a single query would either fabricate rows for an empty table or lose a
// counter to an inner join.
const (
	sqlHealthCycles = `SELECT COUNT(*),` +
		` COALESCE(SUM(contradictions_detected), 0),` +
		` COALESCE(SUM(promotions), 0),` +
		` COALESCE(SUM(demotions), 0),` +
		` COALESCE(SUM(expirations), 0)` +
		` FROM memory_health` +
		` WHERE cycle_at >= pg_now_text('-7 days')`

	sqlHealthNewMemories = `SELECT COUNT(*) FROM memories` +
		` WHERE created_at >= pg_now_text('-7 days')`

	sqlHealthL1Eligible = `SELECT COUNT(*) FROM memories` +
		` WHERE tier = 'L1'` +
		`   AND (use_count >= $1 OR confidence >= $2)`

	// A stale L2 row is one never used, used-but-blank, or last used beyond the
	// window. The empty-string arm is not redundant: this column stores
	// timestamps as text, and '' sorts below every real timestamp, so a row
	// carrying it would count as fresh under a bare comparison.
	sqlHealthL2Totals = `SELECT COUNT(*),` +
		` SUM(CASE WHEN last_used_at IS NULL OR last_used_at = ''` +
		`          OR last_used_at < pg_now_text('-30 days')` +
		`          THEN 1 ELSE 0 END)` +
		` FROM memories WHERE tier = 'L2'`

	sqlStatsByTier  = `SELECT tier, COUNT(*) FROM memories GROUP BY tier`
	sqlStatsByKind  = `SELECT kind, COUNT(*) FROM memories GROUP BY kind`
	sqlStatsConflic = `SELECT COUNT(*) FROM memory_conflicts WHERE resolved = 0`
)

// statsTiers and statsKinds fix the wire's slot order.
//
// The reply is positional, so these orders are the contract: moving an entry
// silently reattributes every count in the array. They are listed once here
// rather than open-coded in a chain of comparisons, which is what the C source
// does and what made the order easy to get wrong.
var (
	statsTiers = [db2contract.StatsCountsTiers]string{"L0", "L1", "L2", "L3", "L4", "L5"}
	statsKinds = [db2contract.StatsCountsKinds]string{
		"fact", "preference", "decision", "episode", "task",
		"scratch", "procedure", "policy", "workflow", "opinion",
	}
)

func (b *pgMemoryBackend) HealthCounters(ctx context.Context, promoteUseCount uint32, promoteConfidence float64) (db2contract.HealthCounters, error) {
	counters := db2contract.HealthCounters{}

	var cycles, contradictions, promotions, demotions, expirations int64
	if err := b.scanOne(ctx, sqlHealthCycles,
		[]any{&cycles, &contradictions, &promotions, &demotions, &expirations}); err != nil {
		if !isNoRows(err) {
			return counters, err
		}
	}

	newMemories, err := b.count(ctx, sqlHealthNewMemories)
	if err != nil {
		return counters, err
	}

	l1Eligible, err := b.count(ctx, sqlHealthL1Eligible, int64(promoteUseCount), promoteConfidence)
	if err != nil {
		return counters, err
	}

	// SUM over an empty tier is NULL, so the stale count is nullable while the
	// COUNT beside it is not.
	var l2Total int64
	var l2Stale *int64
	if err := b.scanOne(ctx, sqlHealthL2Totals, []any{&l2Total, &l2Stale}); err != nil {
		if !isNoRows(err) {
			return counters, err
		}
	}

	fields := []struct {
		target *uint32
		value  int64
	}{
		{&counters.Cycles, cycles},
		{&counters.TotalContradictions, contradictions},
		{&counters.TotalPromotions, promotions},
		{&counters.TotalDemotions, demotions},
		{&counters.TotalExpirations, expirations},
		{&counters.L2Total, l2Total},
	}
	for _, field := range fields {
		narrowed, err := narrowCounter(field.value)
		if err != nil {
			return db2contract.HealthCounters{}, err
		}
		*field.target = narrowed
	}
	counters.NewMemories = newMemories
	counters.L1Eligible = l1Eligible
	if l2Stale != nil {
		narrowed, err := narrowCounter(*l2Stale)
		if err != nil {
			return db2contract.HealthCounters{}, err
		}
		counters.L2Stale30Days = narrowed
	}
	return counters, nil
}

// narrowCounter range-checks a server-side counter into the wire's u32.
//
// The C wrapper refuses the whole operation when any single counter is out of
// range rather than clamping it, and that choice is preserved: a clamped
// counter is a wrong rate presented as a right one, and these feed the host's
// health rates.
func narrowCounter(value int64) (uint32, error) {
	if value < 0 || value > int64(db2contract.HealthCountersMax) {
		return 0, fmt.Errorf("db2: health counter %d outside the contract's range", value)
	}
	return uint32(value), nil
}

func (b *pgMemoryBackend) StatsCounts(ctx context.Context) (db2contract.MemoryStats, error) {
	stats := db2contract.MemoryStats{}

	tierCounts, err := b.groupedCounts(ctx, sqlStatsByTier)
	if err != nil {
		return stats, err
	}
	// The total is summed over every group the database returned, including
	// tiers the contract has no slot for. Summing only the known slots would
	// under-report the corpus the moment a new tier appears, and the total is
	// what callers reconcile against.
	var total int64
	for _, count := range tierCounts {
		total += count
	}
	for index, tier := range statsTiers {
		narrowed, err := narrowCounter(tierCounts[tier])
		if err != nil {
			return db2contract.MemoryStats{}, err
		}
		stats.TierCounts[index] = narrowed
	}

	kindCounts, err := b.groupedCounts(ctx, sqlStatsByKind)
	if err != nil {
		return stats, err
	}
	for index, kind := range statsKinds {
		narrowed, err := narrowCounter(kindCounts[kind])
		if err != nil {
			return db2contract.MemoryStats{}, err
		}
		stats.KindCounts[index] = narrowed
	}

	narrowedTotal, err := narrowCounter(total)
	if err != nil {
		return db2contract.MemoryStats{}, err
	}
	stats.Total = narrowedTotal

	conflicts, err := b.count(ctx, sqlStatsConflic)
	if err != nil {
		return db2contract.MemoryStats{}, err
	}
	stats.Conflicts = conflicts
	return stats, nil
}

// groupedCounts reads a `SELECT label, COUNT(*) … GROUP BY label` into a map.
func (b *pgMemoryBackend) groupedCounts(ctx context.Context, query string) (map[string]int64, error) {
	if b == nil || b.query == nil {
		return nil, ErrNoQuerier
	}
	rows, err := b.query(ctx, query)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	counts := map[string]int64{}
	for rows.Next() {
		var label *string
		var count int64
		if err := rows.Scan(&label, &count); err != nil {
			return nil, err
		}
		// A NULL label is a real row with a real count. It is skipped for slot
		// assignment, exactly as the C source skips it, but the caller still
		// counts it toward the total.
		if label == nil {
			counts[""] += count
			continue
		}
		counts[*label] += count
	}
	return counts, rows.Err()
}
