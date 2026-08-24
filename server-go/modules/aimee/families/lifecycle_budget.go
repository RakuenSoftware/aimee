package families

import (
	"context"
	"math"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

// The work-item budget: a lease-based, exactly-once reservation over a tree of
// work items that share one cap.
//
// The states a reservation moves through, and why each exists:
//
//	''           no reservation.
//	'reserved'   an ESTIMATE is held. The invocation has not been dispatched, or
//	             has not come back, and the money is provisionally spoken for.
//	'unresolved' the invocation crossed the provider boundary and may have spent
//	             an unknown amount. The authorization is RETAINED rather than
//	             released -- releasing it would let the tree hand the same money
//	             out twice.
//	'actual'     a measured cost has replaced the estimate. Terminal.
//
// A lease bounds how long one owner may hold a reservation without being heard
// from, so a crashed process cannot strand a tree's budget forever.
const (
	opWFEBudgetReserve   uint32 = 47
	opWFEBudgetTotals    uint32 = 48
	opWFEBudgetRelease   uint32 = 49
	opWFEBudgetHeartbeat uint32 = 50
	opWFEBudgetReconcile uint32 = 51

	budgetLeaseMinutes = 2

	reservationNone       = ""
	reservationReserved   = "reserved"
	reservationUnresolved = "unresolved"
	reservationActual     = "actual"
)

// reservationHeld are the states that represent money already authorised.
var reservationHeld = []string{reservationReserved, reservationActual, reservationUnresolved}

// reservationReplaceable are the states a measured actual may replace.
var reservationReplaceable = []string{reservationReserved, reservationUnresolved}

const (
	// budgetRootSQL climbs to the root of the tree, taking its cap.
	//
	// UNION rather than UNION ALL, and NO depth column. A recursive CTE stops
	// when an iteration yields no new rows; on a cycle these rows repeat
	// exactly, so the dedup is what terminates it. Carrying a depth would make
	// every row distinct and defeat the very mechanism that stops the walk.
	budgetRootSQL = `WITH RECURSIVE ancestors(id, parent_id, max_usd) AS (
	        SELECT work_item_id, parent_id, work_item_max_cost_usd
	          FROM lifecycle_work_item WHERE work_item_id = $1
	        UNION
	        SELECT parent.work_item_id, parent.parent_id, parent.work_item_max_cost_usd
	          FROM lifecycle_work_item parent
	          JOIN ancestors child ON child.parent_id = parent.work_item_id
	    )
	    SELECT id, max_usd FROM ancestors WHERE parent_id = '' LIMIT 1`

	// lockRootSQL serialises every reservation within one tree.
	//
	// The C opened BEGIN IMMEDIATE, which locks the WHOLE database: two work
	// items in unrelated trees could not reserve at the same time, and a
	// contender got an immediate failure and a backoff loop. Locking the root
	// row keeps exactly the invariant that mattered -- one reservation decision
	// at a time per tree, so two siblings cannot both compute a share from the
	// same availability -- while letting unrelated trees proceed, and it blocks
	// rather than failing, so the retry loop is gone.
	lockRootSQL = `SELECT 1 FROM lifecycle_work_item WHERE work_item_id = $1 FOR UPDATE`

	// treeAvailabilitySQL is what a fair share is computed from.
	treeAvailabilitySQL = `WITH RECURSIVE tree(id) AS (
	        SELECT $1::text
	        UNION
	        SELECT child.work_item_id
	          FROM lifecycle_work_item child
	          JOIN tree parent ON child.parent_id = parent.id
	    )
	    SELECT coalesce(sum(cum_cost_usd), 0),
	           coalesce(sum(reserved_cost_usd), 0),
	           count(*) FILTER (WHERE state = 'active' AND pause_reason = ''
	                              AND reserved_cost_usd = 0)
	      FROM lifecycle_work_item
	     WHERE work_item_id IN (SELECT id FROM tree)`

	treeSpentSQL = `WITH RECURSIVE tree(id) AS (
	        SELECT $1::text
	        UNION
	        SELECT w.work_item_id
	          FROM lifecycle_work_item w JOIN tree t ON w.parent_id = t.id
	    )
	    SELECT coalesce(sum(cum_cost_usd), 0)
	      FROM lifecycle_work_item WHERE work_item_id IN (SELECT id FROM tree)`

	// treeSpentExceptSQL is the reconcile's view: everything the tree has spent,
	// plus everything OTHER items have outstanding.
	treeSpentExceptSQL = `WITH RECURSIVE tree(id) AS (
	        SELECT $1::text
	        UNION
	        SELECT child.work_item_id
	          FROM lifecycle_work_item child
	          JOIN tree parent ON child.parent_id = parent.id
	    )
	    SELECT coalesce(sum(cum_cost_usd), 0),
	           coalesce(sum(reserved_cost_usd) FILTER (WHERE work_item_id <> $2), 0)
	      FROM lifecycle_work_item WHERE work_item_id IN (SELECT id FROM tree)`

	// loadRunnableSQL reads the row only if it may take a reservation at all: a
	// paused or terminal run has no business holding one.
	loadRunnableSQL = `SELECT reserved_cost_usd, reservation_state, reservation_owner
	                     FROM lifecycle_work_item
	                    WHERE work_item_id = $1 AND state = 'active' AND pause_reason = ''`

	loadReservationSQL = `SELECT reserved_cost_usd, reservation_state, reservation_owner
	                        FROM lifecycle_work_item WHERE work_item_id = $1`

	leaseIsLiveSQL = `SELECT reservation_lease_until IS NOT NULL
	                         AND reservation_lease_until > now()
	                    FROM lifecycle_work_item WHERE work_item_id = $1`

	// takeOverSQL adopts a held reservation whose owner has gone quiet. The
	// lease predicate is what makes the takeover legitimate rather than a theft.
	takeOverSQL = `UPDATE lifecycle_work_item
	                  SET reservation_owner = $1,
	                      reservation_lease_until = now() + make_interval(mins => $5)
	                WHERE work_item_id = $2 AND reservation_state = $3
	                  AND reservation_owner = $4
	                  AND (reservation_owner = $1
	                       OR reservation_lease_until IS NULL
	                       OR reservation_lease_until <= now())`

	// stealExpiredSQL takes an expired ESTIMATE and retains it as unresolved:
	// the invocation may have crossed the provider boundary, so the money stays
	// authorised and only a durable replay may resolve it.
	stealExpiredSQL = `UPDATE lifecycle_work_item
	                      SET reservation_state = 'unresolved', reservation_owner = $1,
	                          reservation_lease_until = now() + make_interval(mins => $4)
	                    WHERE work_item_id = $2 AND reservation_state = 'reserved'
	                      AND reservation_owner = $3
	                      AND reservation_lease_until IS NOT NULL
	                      AND reservation_lease_until <= now()`

	touchLeaseSQL = `UPDATE lifecycle_work_item
	                    SET reservation_lease_until = now() + make_interval(mins => $3)
	                  WHERE work_item_id = $1 AND reservation_owner = $2`

	reserveUncappedSQL = `UPDATE lifecycle_work_item
	                         SET reserved_cost_usd = 0, reservation_state = 'reserved',
	                             reservation_owner = $1,
	                             reservation_lease_until = now() + make_interval(mins => $3)
	                       WHERE work_item_id = $2 AND state = 'active' AND pause_reason = ''
	                         AND reservation_state = ''`

	reserveShareSQL = `UPDATE lifecycle_work_item
	                      SET reserved_cost_usd = $1, reservation_state = 'reserved',
	                          reservation_owner = $2,
	                          reservation_lease_until = now() + make_interval(mins => $4)
	                    WHERE work_item_id = $3 AND state = 'active' AND pause_reason = ''
	                      AND reserved_cost_usd = 0`

	// releaseSQL drops only a live ESTIMATE. An actual or unresolved reservation
	// is authorised spend that has already happened.
	releaseSQL = `UPDATE lifecycle_work_item
	                 SET reserved_cost_usd = 0, reservation_state = '',
	                     reservation_owner = '', reservation_lease_until = NULL
	               WHERE work_item_id = $1 AND reservation_owner = $2
	                 AND reservation_state = 'reserved'`

	heartbeatSQL = `UPDATE lifecycle_work_item
	                   SET reservation_lease_until = now() + make_interval(mins => $3)
	                 WHERE work_item_id = $1 AND reservation_owner = $2
	                   AND reservation_state = ANY($4)`

	applyActualSQL = `UPDATE lifecycle_work_item
	                     SET reserved_cost_usd = $1, reservation_state = 'actual'
	                   WHERE work_item_id = $2 AND reservation_owner = $3
	                     AND reservation_state = ANY($4)`
)

// budgetReply is the six-cell answer a reservation attempt gives.
func budgetReply(rootID string, maxUSD, amount float64, allowed, busy, replayOnly bool) (uint32, []string, error) {
	return store.StatusOK, []string{
		rootID, store.Ftoa(maxUSD), store.Ftoa(amount),
		store.Btoa(allowed), store.Btoa(busy), store.Btoa(replayOnly),
	}, nil
}

// budgetRoot climbs to the root of the tree and reads its cap.
func budgetRoot(ctx context.Context, q store.Queryer, workItemID string) (string, float64, error) {
	var rootID string
	var maxUSD float64
	err := q.QueryRow(ctx, budgetRootSQL, workItemID).Scan(&rootID, &maxUSD)
	return rootID, maxUSD, err
}

func leaseIsLive(ctx context.Context, q store.Queryer, workItemID string) (bool, error) {
	var live bool
	if err := q.QueryRow(ctx, leaseIsLiveSQL, workItemID).Scan(&live); err != nil {
		return false, err
	}
	return live, nil
}

// wfeBudgetReserve is op 47: take a share of the tree's budget.
//
// The whole decision runs inside one transaction that holds a lock on the
// tree's ROOT, so two work items in the same tree cannot both compute a share
// from the same availability. There is no retry loop: the C needed one because
// SQLite's BEGIN IMMEDIATE fails a contender immediately, and a row lock blocks
// instead.
func wfeBudgetReserve(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, owner := f[0], f[1]
	if workItemID == "" || owner == "" {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	rootID, maxUSD, err := budgetRoot(ctx, tx, workItemID)
	switch {
	case store.IsNoRows(err):
		// No root means no tree: an unknown work item, or a parent chain that
		// does not reach one.
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	var locked int
	if err := tx.QueryRow(ctx, lockRootSQL, rootID).Scan(&locked); err != nil &&
		!store.IsNoRows(err) {
		return 0, nil, err
	}

	var current float64
	var state, holder string
	switch err := tx.QueryRow(ctx, loadRunnableSQL, workItemID).
		Scan(&current, &state, &holder); {
	case store.IsNoRows(err):
		// Paused, terminal or absent. Not runnable, so not reservable.
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	if state != reservationNone {
		return reserveExisting(ctx, tx, workItemID, owner, rootID, maxUSD, current, state, holder)
	}

	if maxUSD <= 0 {
		// Uncapped: nothing to divide, but ownership and a lease are still
		// recorded so the same exactly-once rules apply to the invocation.
		tag, err := tx.Exec(ctx, reserveUncappedSQL, owner, workItemID, budgetLeaseMinutes)
		if err != nil {
			return 0, nil, err
		}
		if tag.RowsAffected() != 1 {
			return store.StatusFailed, nil, nil
		}
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return budgetReply(rootID, maxUSD, 0, true, false, false)
	}

	var spent, outstanding float64
	var runnable int64
	if err := tx.QueryRow(ctx, treeAvailabilitySQL, rootID).
		Scan(&spent, &outstanding, &runnable); err != nil {
		return 0, nil, err
	}
	remaining := maxUSD - spent - outstanding
	if remaining <= 0 {
		// The tree is out of money. Committing rather than rolling back is
		// deliberate: nothing was written, and the refusal is the answer.
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return budgetReply(rootID, maxUSD, 0, false, false, false)
	}
	if runnable < 1 {
		// This item is asking, so there is at least one claimant even if the
		// count says otherwise.
		runnable = 1
	}
	amount := remaining / float64(runnable)

	tag, err := tx.Exec(ctx, reserveShareSQL, amount, owner, workItemID, budgetLeaseMinutes)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusFailed, nil, nil
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return budgetReply(rootID, maxUSD, amount, true, false, false)
}

// reserveExisting handles a work item that already holds a reservation.
func reserveExisting(ctx context.Context, tx store.Tx, workItemID, owner, rootID string,
	maxUSD, current float64, state, holder string) (uint32, []string, error) {

	replayState := state == reservationActual || state == reservationUnresolved
	if replayState {
		// Replay is still exactly-once work: take ownership only from an owner
		// whose lease has lapsed, and hold a fresh lease while replaying so a
		// concurrent process cannot steal the reservation mid-reconciliation.
		if holder != owner {
			live, err := leaseIsLive(ctx, tx, workItemID)
			if err != nil {
				return 0, nil, err
			}
			if live {
				if err := tx.Commit(ctx); err != nil {
					return 0, nil, err
				}
				return budgetReply(rootID, maxUSD, current, false, true, false)
			}
		}
		tag, err := tx.Exec(ctx, takeOverSQL, owner, workItemID, state, holder, budgetLeaseMinutes)
		if err != nil {
			return 0, nil, err
		}
		if tag.RowsAffected() != 1 {
			// Acquired concurrently.
			return store.StatusFailed, nil, nil
		}
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return budgetReply(rootID, maxUSD, current, true, false, true)
	}

	if state == reservationReserved && holder != owner {
		live, err := leaseIsLive(ctx, tx, workItemID)
		if err != nil {
			return 0, nil, err
		}
		if live {
			if err := tx.Commit(ctx); err != nil {
				return 0, nil, err
			}
			return budgetReply(rootID, maxUSD, current, false, true, false)
		}
		// An expired invocation may have crossed the provider boundary. Retain
		// its authorization as unresolved spend and permit only a durable
		// replay. Releasing it instead would let the tree spend the same money
		// twice.
		tag, err := tx.Exec(ctx, stealExpiredSQL, owner, workItemID, holder, budgetLeaseMinutes)
		if err != nil {
			return 0, nil, err
		}
		if tag.RowsAffected() != 1 {
			return store.StatusFailed, nil, nil
		}
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return budgetReply(rootID, maxUSD, current, true, false, true)
	}

	if state == reservationReserved {
		// The holder is asking again: refresh the lease. Best effort -- failing
		// to extend a lease this process already holds is not a reason to
		// refuse it the reservation it already has.
		_, _ = tx.Exec(ctx, touchLeaseSQL, workItemID, owner, budgetLeaseMinutes)
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return budgetReply(rootID, maxUSD, current, true, false, false)
}

// wfeBudgetTotals is op 48.
func wfeBudgetTotals(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	rootID, maxUSD, err := budgetRoot(ctx, q, f[0])
	switch {
	case store.IsNoRows(err):
		return store.StatusMissing, nil, nil
	case err != nil:
		return 0, nil, err
	}
	var spent float64
	if err := q.QueryRow(ctx, treeSpentSQL, rootID).Scan(&spent); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{
		rootID, store.Ftoa(maxUSD), store.Ftoa(spent),
	}, nil
}

// wfeBudgetRelease is op 49.
func wfeBudgetRelease(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, releaseSQL, f[0], f[1]); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeBudgetHeartbeat is op 50.
func wfeBudgetHeartbeat(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if f[0] == "" {
		return store.StatusInvalid, nil, nil
	}
	if _, err := q.Exec(ctx, heartbeatSQL, f[0], f[1], budgetLeaseMinutes, reservationHeld); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, nil, nil
}

// wfeBudgetReconcile is op 51: replace an estimate with a measured actual.
//
// The allowance is recomputed from durable state on EVERY call, replays
// included: a crash between reconciliation and the tree park must not silently
// upgrade a denied over-budget decision into an approved one.
func wfeBudgetReconcile(ctx context.Context, db store.DB, f []string) (uint32, []string, error) {
	workItemID, owner := f[0], f[1]
	actual, okActual := store.Atof(f[2])
	if workItemID == "" || !okActual {
		return store.StatusInvalid, nil, nil
	}
	// A NaN or Inf in a cost column makes every later budget comparison fail
	// and strands the whole tree, so it is refused at the durable boundary.
	if actual < 0 || math.IsNaN(actual) || math.IsInf(actual, 0) {
		return store.StatusInvalid, nil, nil
	}

	tx, err := db.Begin(ctx)
	if err != nil {
		return 0, nil, err
	}
	defer func() { _ = tx.Rollback(ctx) }()

	rootID, maxUSD, err := budgetRoot(ctx, tx, workItemID)
	switch {
	case store.IsNoRows(err):
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	var locked int
	if err := tx.QueryRow(ctx, lockRootSQL, rootID).Scan(&locked); err != nil &&
		!store.IsNoRows(err) {
		return 0, nil, err
	}

	var current float64
	var state, holder string
	switch err := tx.QueryRow(ctx, loadReservationSQL, workItemID).
		Scan(&current, &state, &holder); {
	case store.IsNoRows(err):
		return store.StatusFailed, nil, nil
	case err != nil:
		return 0, nil, err
	}

	if holder != owner {
		// Someone else's reservation: reconciling it would decide the fate of
		// money this caller did not authorise.
		return store.StatusFailed, nil, nil
	}
	replay := state == reservationActual
	if !replay && state != reservationReserved && state != reservationUnresolved {
		return store.StatusFailed, nil, nil
	}
	if replay && current != actual {
		// A replay must reproduce the cost that was already recorded. A
		// different number is not a retry of the same invocation, and accepting
		// it would rewrite history the tree's accounting depends on.
		return store.StatusFailed, nil, nil
	}

	allowed := true
	if maxUSD > 0 {
		var spent, otherReserved float64
		if err := tx.QueryRow(ctx, treeSpentExceptSQL, rootID, workItemID).
			Scan(&spent, &otherReserved); err != nil {
			return 0, nil, err
		}
		allowed = spent+otherReserved+actual <= maxUSD
	}

	if replay {
		// Already recorded: report the allowance and write nothing.
		if err := tx.Commit(ctx); err != nil {
			return 0, nil, err
		}
		return store.StatusOK, []string{store.Btoa(allowed)}, nil
	}

	if _, err := tx.Exec(ctx, applyActualSQL, actual, workItemID, owner,
		reservationReplaceable); err != nil {
		return 0, nil, err
	}
	if err := tx.Commit(ctx); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{store.Btoa(allowed)}, nil
}
