package db2

import (
	"context"
	"fmt"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	db2contract "github.com/JBailes/aimee/server-go/db2"
)

// HealthSnapshot is one row of the health cycle log.
//
// It is the module's own shape rather than the contract's HealthCounters: the
// wire's counters are a rolling aggregate read back out, while this is the
// single cycle being written in, and the two carry different fields.
type HealthSnapshot struct {
	TotalMemories          uint32
	ContradictionsDetected uint32
	Promotions             uint32
	Demotions              uint32
	Expirations            uint32
}

// nowStamp renders the stamp the tier mutations write into updated_at.
func nowStamp(backend MemoryBackend) string {
	if clocked, ok := backend.(interface{ now() time.Time }); ok {
		return nowUTC(clocked.now())
	}
	return nowUTC(time.Now())
}

// sweepWindow renders the day count the expiry and demotion statements expect.
//
// It is "-30", with no unit: those statements append the unit themselves, one
// through string concatenation and one after stripping the sign back off. This
// is not the same spelling as the retention sweep's "-30 days", and the two
// must not be interchanged — either statement fed the other's spelling silently
// stops matching rows rather than failing.
func sweepWindow(days uint32) string {
	return fmt.Sprintf("-%d", days)
}

// expireSweep deletes the scratch tier, then each kind's stale L1 rows.
//
// Provenance goes first at each stage so no memory row outlives the record of
// where it came from. The per-kind loop exists because the idle window is a
// property of the kind, so one blanket statement cannot express it.
func expireSweep(ctx context.Context, invocation bus.ModuleInvocation, backend MemoryBackend) (uint32, uint32, error) {
	if _, err := backend.DeleteL0Provenance(ctx); err != nil {
		return 0, 0, err
	}
	level0, err := backend.DeleteL0(ctx)
	if err != nil {
		return 0, 0, err
	}

	kinds, err := backend.ListKindsInTier(ctx, db2contract.ExpireStaleTier, db2contract.ExpireKindsMax)
	if err != nil {
		return 0, 0, err
	}

	var stale uint32
	for _, kind := range kinds {
		if invocation.Cancelled() || ctx.Err() != nil {
			return 0, 0, ctx.Err()
		}
		days, err := backend.KindExpireDays(ctx, kind)
		if err != nil {
			return 0, 0, err
		}
		// A zero window would expire everything of this kind immediately. The
		// C implementation refuses the whole sweep rather than proceed, and so
		// does this: a missing policy is not licence to delete.
		if days == 0 {
			return 0, 0, fmt.Errorf("db2: kind %q has no expiry window", kind)
		}
		window := sweepWindow(days)
		if _, err := backend.DeleteStaleL1Provenance(ctx, kind, window); err != nil {
			return 0, 0, err
		}
		deleted, err := backend.DeleteStaleL1(ctx, kind, window)
		if err != nil {
			return 0, 0, err
		}
		stale += deleted
		if stale > db2contract.ExpireMax {
			return 0, 0, fmt.Errorf("db2: expiry swept past the contract's bound")
		}
	}
	return level0, stale, nil
}

// demoteSweep demotes each kind's qualifying L2 rows, then cascades once.
//
// One stamp covers the whole action so the cascade matches dependants of
// exactly the rows this call demoted. Re-reading the clock per kind would let
// the cascade miss rows demoted a tick earlier, or catch rows demoted by a
// concurrent sweep.
func demoteSweep(ctx context.Context, invocation bus.ModuleInvocation, backend MemoryBackend) (uint32, uint32, error) {
	stamp := nowStamp(backend)

	kinds, err := backend.ListKindsInTier(ctx, db2contract.DemoteTier, db2contract.DemoteKindsMax)
	if err != nil {
		return 0, 0, err
	}

	var demoted uint32
	for _, kind := range kinds {
		if invocation.Cancelled() || ctx.Err() != nil {
			return 0, 0, ctx.Err()
		}
		confidence, days, err := backend.KindDemotePolicy(ctx, kind)
		if err != nil {
			return 0, 0, err
		}
		if days == 0 {
			return 0, 0, fmt.Errorf("db2: kind %q has no demotion window", kind)
		}
		changed, err := backend.DemoteKind(ctx, stamp, kind, confidence, sweepWindow(days))
		if err != nil {
			return 0, 0, err
		}
		demoted += changed
		if demoted > db2contract.DemoteMax {
			return 0, 0, fmt.Errorf("db2: demotion swept past the contract's bound")
		}
	}

	// Nothing demoted means nothing to cascade to. The contract refuses a reply
	// carrying cascaded rows with none demoted, so skipping the call is what
	// keeps the two consistent rather than merely saving a round trip.
	var cascaded uint32
	if demoted > 0 {
		if invocation.Cancelled() || ctx.Err() != nil {
			return 0, 0, ctx.Err()
		}
		cascaded, err = backend.DemoteCascade(ctx, stamp)
		if err != nil {
			return 0, 0, err
		}
	}
	return demoted, cascaded, nil
}
