package families

import (
	"context"
	"fmt"
	"strconv"

	store "github.com/JBailes/aimee/server-go/modules/aimee"
)

const opSessionIRCancel uint32 = 53
const operatorCancelPrefix = "operator-cancel:"
const operatorCancelPayload = `{"kind":"execution","payload":{"status":"cancelled","error":"Operator cancelled continuation; any in-flight tool effects remain unresolved until independently reconciled."}}`

const sessionTurnOpenSQL = `SELECT
 (SELECT count(*) FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND actor=$3 AND payload::jsonb->>'kind'='execution' AND payload::jsonb->'payload'->>'status'='running')=1
 AND NOT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND payload::jsonb->>'kind'='execution' AND payload::jsonb->'payload'->>'status'<>'running')`

// Serialize cancellation with legacy unbound admission across projects. Bound
// calls already share the exact canonical destination row lock.
func sessionOperatorCancelLock(ctx context.Context, q store.Queryer, actor string) error {
	var one int
	return q.QueryRow(ctx, `SELECT 1 FROM (SELECT pg_advisory_xact_lock(hashtextextended('session-operator-cancel:' || $1,0))) AS held`, actor).Scan(&one)
}

// Recognize only the exact owner-bound terminal emitted by this operation;
// historic arbitrary event IDs sharing the prefix do not acquire authority.
const sessionOperatorCancelledSQL = `SELECT EXISTS(SELECT 1 FROM session_ir_events e JOIN server_sessions s ON s.id=e.session_id AND s.principal=e.actor
 WHERE e.actor=$1 AND e.event_id='operator-cancel:' || e.turn_id AND e.payload='` + operatorCancelPayload + `')`

// Only the host's authenticated browser-operator route invokes this operation.
// It cancels continuation, not process custody or an already dispatched effect.
// The immutable terminal itself fences future writes; no new lease/liveness
// assertion or absence of a process-local worker authorizes this transaction.
func sessionIRCancel(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 3 || f[0] == "" || len(f[0]) >= 160 || f[1] == "" || len(f[1]) >= 128 || f[2] == "" || len(f[2]) >= 128 {
		return store.StatusInvalid, nil, nil
	}
	actor, session, turn := f[0], f[1], f[2]
	if err := sessionOperatorCancelLock(ctx, q, actor); err != nil {
		return 0, nil, err
	}
	status, err := nativeOwnerLock(ctx, q, actor, session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	id := operatorCancelPrefix + turn
	var sequence int64
	var priorTurn, priorActor, priorPayload string
	err = q.QueryRow(ctx, sessionIRExistingSQL, session, id).Scan(&sequence, &priorTurn, &priorActor, &priorPayload)
	if err == nil {
		if priorTurn != turn || priorActor != actor || priorPayload != operatorCancelPayload {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{strconv.FormatInt(sequence, 10)}, nil
	}
	if !store.IsNoRows(err) {
		return 0, nil, err
	}
	var open bool
	if err = q.QueryRow(ctx, sessionTurnOpenSQL, session, turn, actor).Scan(&open); err != nil {
		return 0, nil, err
	}
	if !open {
		return store.StatusInvalid, nil, nil
	}
	var native bool
	err = q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM session_native_reservations WHERE session_id=$1 AND state='active') OR EXISTS(SELECT 1 FROM session_native_cache_owners c WHERE c.session_id=$1 AND c.terminal_sequence IS NULL AND NOT EXISTS(SELECT 1 FROM session_native_reservations r WHERE r.session_id=c.session_id AND r.harness=c.harness AND r.binding_sha256=c.binding_sha256 AND r.owner_id=c.owner_id AND r.turn_id=c.turn_id AND r.actor=c.actor AND r.claim_id=c.claim_id AND r.containment_sha256=c.containment_sha256 AND r.epoch=c.cache_epoch AND r.state IN ('finished','reconciled') AND r.terminal_sequence IS NOT NULL))`, session).Scan(&native)
	if err != nil {
		return 0, nil, err
	}
	if native {
		return store.StatusInvalid, nil, nil
	}
	sources, err := operatorInterruptionSources(ctx, q, session, turn, actor)
	if err != nil {
		return 0, nil, err
	}
	fields := []string{session, id, turn, actor, operatorCancelPayload}
	if err = q.QueryRow(ctx, sessionIRInsertSQL, session, id, turn, actor, operatorCancelPayload).Scan(&sequence); err != nil {
		return 0, nil, err
	}
	if err = enqueueLearningProjection(ctx, q, fields, sequence); err != nil {
		return store.StatusFailed, nil, err
	}
	terminal := strconv.FormatInt(sequence, 10)
	if err = operatorRecordInterruptions(ctx, q, session, turn, actor, terminal, sources); err != nil {
		return 0, nil, fmt.Errorf("operator interruption capture: %w", err)
	}
	return store.StatusOK, []string{terminal}, nil
}

// Caller holds the same session row lock as cancellation. A bound action may
// only belong to a real open owned turn. Legacy actions without IR remain
// supported. After any operator cancellation by this actor, unbound admission
// refuses even in another project: absent a canonical tuple there is no durable
// proof that it is independent of the cancelled child. Other actors and fresh
// bound turns remain unaffected.
func sessionActionTurnAdmitted(ctx context.Context, q store.Queryer, session, actor, turn string) (bool, error) {
	var admitted bool
	if turn != "" {
		err := q.QueryRow(ctx, sessionTurnOpenSQL, session, turn, actor).Scan(&admitted)
		return admitted, err
	}
	var cancelled bool
	err := q.QueryRow(ctx, sessionOperatorCancelledSQL, actor).Scan(&cancelled)
	return !cancelled, err
}
