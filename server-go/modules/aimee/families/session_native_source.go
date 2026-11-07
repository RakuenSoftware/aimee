package families

import (
	"bytes"
	"context"
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
)

const opSessionNativeSourceAppend uint32 = 47

type nativeOwnedMessage struct {
	Kind      string                          `json:"kind"`
	MessageID string                          `json:"message_id"`
	Role      string                          `json:"role,omitempty"`
	Payload   canonical.Message               `json:"payload"`
	Owner     *canonical.NativeExecutionOwner `json:"native_owner"`
	Partial   json.RawMessage                 `json:"partial,omitempty"`
}

func nativeOpaqueKey(raw json.RawMessage) []byte {
	decoder := json.NewDecoder(bytes.NewReader(raw))
	decoder.UseNumber()
	var value any
	if decoder.Decode(&value) != nil {
		return nil
	}
	normalized, err := json.Marshal(value)
	if err != nil {
		return nil
	}
	return normalized
}
func sameNativeCall(a, b canonical.Block) bool {
	return a.Type == "tool_use" && b.Type == "tool_use" && a.ToolID == b.ToolID && a.Name == b.Name && a.Namespace == b.Namespace && bytes.Equal(nativeOpaqueKey(a.Input), nativeOpaqueKey(b.Input))
}

// Protected snapshots grow monotonically. Derived classification may augment
// already closed blocks, but an observed tool effect's identity never changes.
func nativeSnapshotExtends(old, next nativeOwnedMessage) bool {
	if old.Owner == nil || next.Owner == nil || *old.Owner != *next.Owner || len(next.Payload.Blocks) < len(old.Payload.Blocks) || next.Payload.Role != old.Payload.Role {
		return false
	}
	for i, b := range old.Payload.Blocks {
		following := next.Payload.Blocks[i]
		b.Context = nil
		b.ThinkingBinding = nil
		following.Context = nil
		following.ThinkingBinding = nil
		original, _ := json.Marshal(b)
		updated, _ := json.Marshal(following)
		if !bytes.Equal(nativeOpaqueKey(original), nativeOpaqueKey(updated)) {
			return false
		}
	}
	return true
}

func sessionNativeSourceAppend(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	if len(f) != 5 || !canonical.ValidSessionEvent([]byte(f[4])) {
		return store.StatusInvalid, nil, nil
	}
	var event nativeOwnedMessage
	if !canonical.Decode([]byte(f[4]), &event) || event.Kind != "message" || event.Owner == nil {
		return store.StatusInvalid, nil, nil
	}
	for _, id := range f[:4] {
		if id == "" || len(id) > 1024 {
			return store.StatusInvalid, nil, nil
		}
	}
	status, err := nativeOwnerLock(ctx, q, f[3], f[0])
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	// Exact immutable retries do not create a new writer after the owner finished.
	var sequence int64
	var turn, actor, payload string
	err = q.QueryRow(ctx, sessionIRExistingSQL, f[0], f[1]).Scan(&sequence, &turn, &actor, &payload)
	if err == nil {
		if turn != f[2] || actor != f[3] || payload != f[4] {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{strconv.FormatInt(sequence, 10)}, nil
	}
	if !store.IsNoRows(err) {
		return 0, nil, err
	}
	owner := event.Owner
	r := nativeReservationRequest{Version: 1, Session: f[0], Turn: f[2], Harness: owner.Harness, Binding: owner.Binding, Owner: owner.Owner, Epoch: owner.Epoch, Containment: owner.Containment}
	v, err := nativeReservationRead(ctx, q, r)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	epoch, err := nativeEpochCurrent(ctx, q, r, f[3])
	if err != nil {
		return 0, nil, err
	}
	if v.State != "active" || v.Actor != f[3] || v.Turn != f[2] || v.Epoch != owner.Epoch || epoch != owner.Epoch || v.Containment != owner.Containment || v.Claim == "" {
		return store.StatusInvalid, nil, nil
	}
	cacheOwner := nativeReservationSource(v)
	registered, principal, _, _, err := nativeOwnerRead(ctx, q, cacheOwner)
	if err != nil {
		return 0, nil, err
	}
	if principal != f[3] || !nativeOwnerSame(registered, cacheOwner) {
		return store.StatusInvalid, nil, nil
	}
	token, err := nativeOwnerCacheToken(ctx, q, cacheOwner, f[3])
	if err != nil {
		return 0, nil, err
	}
	phase, claim, claimTurn, valid := nativeToken(token)
	if !valid || phase != "pending" || claim != v.Claim || claimTurn != v.Turn {
		return store.StatusInvalid, nil, nil
	}
	state, _, _, err := nativeOwnerLifecycle(ctx, q, f[3], cacheOwner)
	if err != nil {
		return 0, nil, err
	}
	if state != "running" {
		return store.StatusInvalid, nil, nil
	}
	var previous string
	err = q.QueryRow(ctx, `SELECT payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND payload::jsonb->>'kind'='message' AND payload::jsonb->>'message_id'=$3 ORDER BY session_ir_events.sequence DESC LIMIT 1`, f[0], f[2], event.MessageID).Scan(&previous)
	if err == nil {
		var old nativeOwnedMessage
		if !canonical.Decode([]byte(previous), &old) || !nativeSnapshotExtends(old, event) {
			return store.StatusInvalid, nil, nil
		}
	} else if !store.IsNoRows(err) {
		return 0, nil, err
	}
	return sessionIRAppendAuthorized(ctx, q, f, false, false, true)
}
