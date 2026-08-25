package db2

import (
	"context"
	"math"
)

// The default lifecycle for a kind the table does not describe.
//
// These match the original fact thresholds, and they are applied on every miss
// rather than failing the lookup: a kind nobody has tuned still has to age,
// and refusing to sweep it would let an untuned kind accumulate forever.
const (
	defaultPromoteUseCount  = 3
	defaultPromoteConfid    = 0.9
	defaultDemoteDays       = 60
	defaultDemoteConfidence = 0.7
	defaultExpireDays       = 30
	defaultDemoteResistance = 1.0
)

const sqlKindLifecycle = `SELECT promote_use_count, promote_confidence,` +
	` demote_days, demote_confidence, expire_days,` +
	` demotion_resistance` +
	` FROM kind_lifecycle WHERE kind = $1`

// kindLifecycle is one row of the per-kind policy table.
type kindLifecycle struct {
	PromoteUseCount    int64
	PromoteConfidence  float64
	DemoteDays         int64
	DemoteConfidence   float64
	ExpireDays         int64
	DemotionResistance float64
}

func defaultKindLifecycle() kindLifecycle {
	return kindLifecycle{
		PromoteUseCount:    defaultPromoteUseCount,
		PromoteConfidence:  defaultPromoteConfid,
		DemoteDays:         defaultDemoteDays,
		DemoteConfidence:   defaultDemoteConfidence,
		ExpireDays:         defaultExpireDays,
		DemotionResistance: defaultDemoteResistance,
	}
}

// loadKindLifecycle reads a kind's policy, falling back to the defaults.
//
// A miss is not an error. A query failure is not either: the C implementation
// leaves the defaults in place and reports the miss through a return value its
// only two callers deliberately ignore, because a policy lookup that fails is
// still better answered with a usable window than with a refusal to sweep.
func (b *pgMemoryBackend) loadKindLifecycle(ctx context.Context, kind string) kindLifecycle {
	lifecycle := defaultKindLifecycle()
	if kind == "" || b == nil || b.queryRow == nil {
		return lifecycle
	}
	row := b.queryRow(ctx, sqlKindLifecycle, kind)
	if row == nil {
		return lifecycle
	}
	loaded := kindLifecycle{}
	if err := row.Scan(&loaded.PromoteUseCount, &loaded.PromoteConfidence,
		&loaded.DemoteDays, &loaded.DemoteConfidence, &loaded.ExpireDays,
		&loaded.DemotionResistance); err != nil {
		return lifecycle
	}
	return loaded
}

// KindExpireDays answers the idle window after which a kind's L1 rows expire.
func (b *pgMemoryBackend) KindExpireDays(ctx context.Context, kind string) (uint32, error) {
	days := b.loadKindLifecycle(ctx, kind).ExpireDays
	if days <= 0 {
		// A stored zero or negative window would expire the kind immediately.
		// The default is used instead, which is the same window an undescribed
		// kind gets.
		days = defaultExpireDays
	}
	return uint32(days), nil
}

// KindDemotePolicy answers the confidence floor and idle window for demotion.
//
// Resistance stretches the idle window a kind must sit through before it
// demotes; it lives with the thresholds it scales, so it is applied here rather
// than left for the caller to remember.
func (b *pgMemoryBackend) KindDemotePolicy(ctx context.Context, kind string) (float64, uint32, error) {
	lifecycle := b.loadKindLifecycle(ctx, kind)
	days := lifecycle.DemoteDays
	if days <= 0 {
		days = defaultDemoteDays
	}
	resistance := lifecycle.DemotionResistance
	// A zero, negative or non-finite resistance would collapse the window to
	// nothing or to nonsense. Neither is a policy, so the unscaled window is
	// used instead.
	if resistance <= 0 || math.IsInf(resistance, 0) || math.IsNaN(resistance) {
		resistance = defaultDemoteResistance
	}
	scaled := float64(days) * resistance
	if scaled < 1 {
		scaled = 1
	}
	if scaled > math.MaxUint32 {
		scaled = math.MaxUint32
	}
	return lifecycle.DemoteConfidence, uint32(scaled), nil
}
