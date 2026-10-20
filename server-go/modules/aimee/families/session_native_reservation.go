package families

import (
	"context"
	"crypto/rand"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
)

const (
	opSessionNativeReserve              uint32 = 40
	opSessionNativeReservationReconcile uint32 = 41
	opSessionNativeEpoch                uint32 = 42
	opSessionNativeReservationLookup    uint32 = 43
)

const opSessionNativeReservationFinish uint32 = 44

type nativeReservationRequest struct {
	Version     int    `json:"version"`
	Session     string `json:"session_id"`
	Turn        string `json:"turn_id,omitempty"`
	Harness     string `json:"harness"`
	Binding     string `json:"binding_sha256"`
	Owner       string `json:"owner_id,omitempty"`
	Containment string `json:"containment_sha256,omitempty"`
	Descriptor  string `json:"containment_json,omitempty"`
	Epoch       string `json:"epoch,omitempty"`
	Expected    string `json:"expected_token,omitempty"`
}
type nativeReservation struct {
	nativeReservationRequest
	Actor         string `json:"actor"`
	Claim         string `json:"claim_id,omitempty"`
	Scope         string `json:"scope_sha256,omitempty"`
	Context       string `json:"context_sha256,omitempty"`
	State         string `json:"state"`
	Terminal      string `json:"terminal_sequence,omitempty"`
	RecoveryEpoch string `json:"recovery_epoch,omitempty"`
	RecoveredFrom string `json:"recovered_from,omitempty"`
	FinishedFrom  string `json:"finished_from,omitempty"`
}

func nativeEpochValid(epoch string) bool { return epoch == "0" || nativeHex(epoch, 32) }
func nativeReservationInput(f []string) (nativeReservationRequest, bool) {
	var r nativeReservationRequest
	if len(f) != 2 || f[0] == "" || len(f[0]) > 128 || len(f[1]) > 16384 || !canonical.Decode([]byte(f[1]), &r) {
		return r, false
	}
	return r, r.Version == 1 && r.Session != "" && len(r.Session) <= 128 && (r.Harness == "codex" || r.Harness == "claude") && nativeHex(r.Binding, 64)
}
func nativeEpochCurrent(ctx context.Context, q store.Queryer, r nativeReservationRequest, actor string) (string, error) {
	if _, err := q.Exec(ctx, `INSERT INTO session_native_cache_epochs(session_id,harness,binding_sha256,actor) VALUES($1,$2,$3,$4) ON CONFLICT DO NOTHING`, r.Session, r.Harness, r.Binding, actor); err != nil {
		return "", err
	}
	var epoch, owner string
	err := q.QueryRow(ctx, `SELECT epoch,actor FROM session_native_cache_epochs WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3`, r.Session, r.Harness, r.Binding).Scan(&epoch, &owner)
	if err != nil {
		return "", err
	}
	if owner != actor || !nativeEpochValid(epoch) {
		return "", fmt.Errorf("invalid native cache epoch ownership")
	}
	return epoch, nil
}
func sessionNativeEpoch(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, ok := nativeReservationInput(f)
	if !ok || r.Turn != "" || r.Owner != "" || r.Containment != "" || r.Descriptor != "" || r.Epoch != "" || r.Expected != "" {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	epoch, err := nativeEpochCurrent(ctx, q, r, f[0])
	return store.StatusOK, []string{epoch}, err
}

const nativeReservationReadSQL = `SELECT turn_id,actor,epoch,containment_sha256,containment_json,claim_id,scope_sha256,context_sha256,state,coalesce(terminal_sequence,0)::text,recovery_epoch,recovered_from,finished_from FROM session_native_reservations WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND owner_id=$4`

func nativeReservationRead(ctx context.Context, q store.Queryer, r nativeReservationRequest) (nativeReservation, error) {
	v := nativeReservation{nativeReservationRequest: r}
	err := q.QueryRow(ctx, nativeReservationReadSQL, r.Session, r.Harness, r.Binding, r.Owner).Scan(&v.Turn, &v.Actor, &v.Epoch, &v.Containment, &v.Descriptor, &v.Claim, &v.Scope, &v.Context, &v.State, &v.Terminal, &v.RecoveryEpoch, &v.RecoveredFrom, &v.FinishedFrom)
	if err == nil {
		sum := sha256.Sum256([]byte(v.Descriptor))
		if v.Actor == "" || v.Turn == "" || len(v.Turn) > 128 || !nativeEpochValid(v.Epoch) || !nativeHex(v.Containment, 64) || hex.EncodeToString(sum[:]) != v.Containment || len(v.Descriptor) > 8192 || !canonical.ValidJSON([]byte(v.Descriptor)) || (v.State != "active" && v.State != "reconciled" && v.State != "finished") {
			return v, fmt.Errorf("invalid persisted native reservation")
		}
	}
	return v, err
}
func nativeReservationSource(r nativeReservation) nativeOwnerRequest {
	return nativeOwnerRequest{Version: 1, Session: r.Session, Turn: r.Turn, Harness: r.Harness, Binding: r.Binding, Owner: r.Owner, Containment: r.Containment, Claim: r.Claim, Scope: r.Scope, Context: r.Context, Epoch: r.Epoch}
}
func sessionNativeReserve(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, ok := nativeReservationInput(f)
	sum := sha256.Sum256([]byte(r.Descriptor))
	if !ok || r.Turn == "" || len(r.Turn) > 128 || !nativeHex(r.Owner, 32) || !nativeHex(r.Containment, 64) || r.Descriptor == "" || len(r.Descriptor) > 8192 || !canonical.ValidJSON([]byte(r.Descriptor)) || hex.EncodeToString(sum[:]) != r.Containment || (r.Epoch != "" && !nativeEpochValid(r.Epoch)) || r.Expected != "" {
		return store.StatusInvalid, nil, nil
	}
	var object map[string]json.RawMessage
	if json.Unmarshal([]byte(r.Descriptor), &object) != nil || object == nil {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	epoch, err := nativeEpochCurrent(ctx, q, r, f[0])
	if err != nil {
		return 0, nil, err
	}
	if r.Epoch != "" && r.Epoch != epoch {
		return store.StatusMissing, nil, nil
	}
	r.Epoch = epoch
	fenced, err := nativeRecoveryFenced(ctx, q, r.Session, r.Turn)
	if err != nil {
		return 0, nil, err
	}
	if fenced {
		return store.StatusInvalid, nil, nil
	}
	state, _, _, err := nativeOwnerLifecycle(ctx, q, f[0], nativeOwnerRequest{Session: r.Session, Turn: r.Turn})
	if err != nil {
		return 0, nil, err
	}
	if state != "running" {
		return store.StatusInvalid, nil, nil
	}
	prior, err := nativeReservationRead(ctx, q, r)
	if err == nil {
		if prior.Actor == f[0] && prior.Turn == r.Turn && prior.Epoch == epoch && prior.Containment == r.Containment && prior.Descriptor == r.Descriptor && prior.State == "active" {
			return store.StatusOK, []string{epoch}, nil
		}
		return store.StatusInvalid, nil, nil
	}
	if !store.IsNoRows(err) {
		return 0, nil, err
	}
	var active string
	err = q.QueryRow(ctx, `SELECT owner_id FROM session_native_reservations WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND state='active'`, r.Session, r.Harness, r.Binding).Scan(&active)
	if err == nil {
		return store.StatusMissing, nil, nil
	}
	if !store.IsNoRows(err) {
		return 0, nil, err
	}
	_, err = q.Exec(ctx, `INSERT INTO session_native_reservations(session_id,harness,binding_sha256,owner_id,turn_id,actor,epoch,containment_sha256,containment_json) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9)`, r.Session, r.Harness, r.Binding, r.Owner, r.Turn, f[0], epoch, r.Containment, r.Descriptor)
	return store.StatusOK, []string{epoch}, err
}
func nativeReservationClaimCheck(ctx context.Context, q store.Queryer, r nativeOwnerRequest, actor string) error {
	v, err := nativeReservationRead(ctx, q, nativeReservationRequest{Version: 1, Session: r.Session, Harness: r.Harness, Binding: r.Binding, Owner: r.Owner})
	if err != nil {
		return err
	}
	epoch, err := nativeEpochCurrent(ctx, q, v.nativeReservationRequest, actor)
	if err != nil {
		return err
	}
	if v.Actor != actor || v.Turn != r.Turn || v.Containment != r.Containment || v.State != "active" || v.Epoch != r.Epoch || epoch != r.Epoch || (v.Claim != "" && (v.Claim != r.Claim || v.Scope != r.Scope || v.Context != r.Context)) {
		return fmt.Errorf("native cache claim does not match pre-exec reservation")
	}
	return nil
}
func nativeReservationBindClaim(ctx context.Context, q store.Queryer, r nativeOwnerRequest) error {
	_, err := q.Exec(ctx, `UPDATE session_native_reservations SET claim_id=$5,scope_sha256=$6,context_sha256=$7 WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND owner_id=$4`, r.Session, r.Harness, r.Binding, r.Owner, r.Claim, r.Scope, r.Context)
	return err
}
func nativeReservationFence(ctx context.Context, q store.Queryer, v nativeReservation) (string, error) {
	r := nativeReservationSource(v)
	var actor, owner, claim, containment, terminal string
	err := q.QueryRow(ctx, `SELECT actor,owner_id,claim_id,containment_sha256,terminal_sequence::text FROM session_native_recovery_fences WHERE session_id=$1 AND turn_id=$2`, r.Session, r.Turn).Scan(&actor, &owner, &claim, &containment, &terminal)
	if err == nil {
		if actor != v.Actor || owner != r.Owner || claim != r.Claim || containment != r.Containment {
			return "", fmt.Errorf("native reservation source fence mismatch")
		}
		state, current, _, err := nativeOwnerLifecycle(ctx, q, v.Actor, r)
		if err != nil {
			return "", err
		}
		if state == "running" || current != terminal {
			return "", fmt.Errorf("native reservation terminal missing")
		}
		return terminal, nil
	}
	if !store.IsNoRows(err) {
		return "", err
	}
	state, terminal, _, err := nativeOwnerLifecycle(ctx, q, v.Actor, r)
	if err != nil {
		return "", err
	}
	if state == "running" {
		id := "native-recovery:" + r.Owner + ":preexec"
		if r.Claim != "" {
			id = "native-recovery:" + r.Owner + ":" + r.Claim
		}
		raw := `{"kind":"execution","payload":{"status":"cancelled","error":"native execution interrupted by server restart"}}`
		status, cells, err := sessionIRAppendInternal(ctx, q, []string{r.Session, id, r.Turn, v.Actor, raw}, false)
		if err != nil {
			return "", err
		}
		if status != store.StatusOK || len(cells) != 1 {
			return "", fmt.Errorf("native reservation cancellation refused")
		}
		terminal = cells[0]
	}
	sequence, err := strconv.ParseInt(terminal, 10, 64)
	if err != nil || sequence < 1 {
		return "", fmt.Errorf("invalid native reservation terminal")
	}
	_, err = q.Exec(ctx, `INSERT INTO session_native_recovery_fences(session_id,turn_id,actor,owner_id,claim_id,containment_sha256,terminal_sequence) VALUES($1,$2,$3,$4,$5,$6,$7)`, r.Session, r.Turn, v.Actor, r.Owner, r.Claim, r.Containment, sequence)
	return terminal, err
}
func sessionNativeReservationReconcile(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, ok := nativeReservationInput(f)
	if !ok || !nativeHex(r.Owner, 32) || r.Turn == "" || len(r.Turn) > 128 || !nativeEpochValid(r.Epoch) || !nativeHex(r.Containment, 64) || r.Descriptor != "" {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	v, err := nativeReservationRead(ctx, q, r)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if v.Actor != f[0] || v.Turn != r.Turn || v.Containment != r.Containment || v.Epoch != r.Epoch {
		return store.StatusInvalid, nil, nil
	}
	epoch, err := nativeEpochCurrent(ctx, q, r, f[0])
	if err != nil {
		return 0, nil, err
	}
	if v.State == "finished" {
		if epoch != v.Epoch || r.Expected != v.FinishedFrom {
			return store.StatusMissing, nil, nil
		}
		state, terminal, _, err := nativeOwnerLifecycle(ctx, q, f[0], nativeReservationSource(v))
		if err != nil {
			return 0, nil, err
		}
		if state == "running" || terminal != v.Terminal {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{terminal, epoch}, nil
	}
	if v.State == "reconciled" {
		if epoch != v.RecoveryEpoch || r.Expected != v.RecoveredFrom {
			return store.StatusMissing, nil, nil
		}
		terminal, err := nativeReservationFence(ctx, q, v)
		if err != nil {
			return 0, nil, err
		}
		if terminal != v.Terminal {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{terminal, v.RecoveryEpoch}, nil
	}
	if epoch != v.Epoch {
		return store.StatusMissing, nil, nil
	}
	ownerRequest := nativeReservationSource(v)
	if v.Claim != "" {
		owner, actor, recovered, _, err := nativeOwnerRead(ctx, q, ownerRequest)
		if err != nil {
			return 0, nil, err
		}
		if actor != f[0] || !nativeOwnerSame(owner, ownerRequest) {
			return store.StatusInvalid, nil, nil
		}
		actual, err := nativeOwnerCacheToken(ctx, q, ownerRequest, f[0])
		if err != nil {
			return 0, nil, err
		}
		phase, claim, turn, valid := nativeToken(actual)
		if !valid || claim != v.Claim || turn != v.Turn || actual != r.Expected {
			return store.StatusMissing, nil, nil
		}
		if recovered != "" && actual != ownerRequest.token("invalid") {
			return store.StatusInvalid, nil, nil
		}
		if phase == "pending" || phase == "finishing" {
			tag, err := q.Exec(ctx, `UPDATE session_harness_handles SET native_id=$4 WHERE session_id=$1 AND harness=$2 AND native_id=$3`, v.Session, ownerRequest.key(), actual, ownerRequest.token("invalid"))
			if err != nil {
				return 0, nil, err
			}
			if tag.RowsAffected() != 1 {
				return store.StatusMissing, nil, nil
			}
		}
	} else if r.Expected != "" {
		return store.StatusInvalid, nil, nil
	}
	terminal, err := nativeReservationFence(ctx, q, v)
	if err != nil {
		return 0, nil, err
	}
	var random [16]byte
	if _, err = rand.Read(random[:]); err != nil {
		return 0, nil, err
	}
	nextEpoch := hex.EncodeToString(random[:])
	if nextEpoch == epoch {
		return 0, nil, fmt.Errorf("native epoch collision")
	}
	tag, err := q.Exec(ctx, `UPDATE session_native_cache_epochs SET epoch=$5 WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND epoch=$4`, r.Session, r.Harness, r.Binding, epoch, nextEpoch)
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusMissing, nil, nil
	}
	_, err = q.Exec(ctx, `UPDATE session_native_reservations SET state='reconciled',terminal_sequence=$5,recovery_epoch=$6,recovered_from=$7 WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND owner_id=$4`, r.Session, r.Harness, r.Binding, r.Owner, terminal, nextEpoch, r.Expected)
	return store.StatusOK, []string{terminal, nextEpoch}, err
}
func sessionNativeReservationLookup(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, ok := nativeReservationInput(f)
	if !ok || (r.Owner != "" && !nativeHex(r.Owner, 32)) || r.Turn != "" || r.Containment != "" || r.Descriptor != "" || r.Epoch != "" || r.Expected != "" {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	// A missing local marker cannot hide an active durable directory owner.
	// The session lock and active-owner unique index make this exact and bounded.
	if r.Owner == "" {
		err = q.QueryRow(ctx, `SELECT owner_id FROM session_native_reservations WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND state='active'`, r.Session, r.Harness, r.Binding).Scan(&r.Owner)
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		if err != nil {
			return 0, nil, err
		}
	}
	v, err := nativeReservationRead(ctx, q, r)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if v.Actor != f[0] {
		return store.StatusInvalid, nil, nil
	}
	if _, _, _, err := nativeOwnerLifecycle(ctx, q, f[0], nativeReservationSource(v)); err != nil {
		return 0, nil, err
	}
	if v.State == "reconciled" {
		v.Expected = v.RecoveredFrom
	} else if v.State == "finished" {
		v.Expected = v.FinishedFrom
	} else if v.Claim != "" {
		request := nativeReservationSource(v)
		owner, actor, _, _, readErr := nativeOwnerRead(ctx, q, request)
		if readErr != nil {
			return 0, nil, readErr
		}
		if actor != f[0] || !nativeOwnerSame(owner, request) {
			return store.StatusInvalid, nil, nil
		}
		v.Expected, err = nativeOwnerCacheToken(ctx, q, request, f[0])
		if err != nil {
			return 0, nil, err
		}
		_, claim, turn, valid := nativeToken(v.Expected)
		if !valid || claim != v.Claim || turn != v.Turn {
			return store.StatusMissing, nil, nil
		}
	}
	encoded, err := json.Marshal(v)
	if len(encoded) > 16384 {
		return 0, nil, fmt.Errorf("native reservation metadata too large")
	}
	return store.StatusOK, []string{string(encoded)}, err
}

// Finish is normal resource cleanup after actual quiescence and an already
// durable terminal. It never cancels a running source or changes cache epoch.
func sessionNativeReservationFinish(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, ok := nativeReservationInput(f)
	if !ok || !nativeHex(r.Owner, 32) || r.Turn == "" || len(r.Turn) > 128 || !nativeEpochValid(r.Epoch) || !nativeHex(r.Containment, 64) || r.Descriptor != "" {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	v, err := nativeReservationRead(ctx, q, r)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if v.Actor != f[0] || v.Turn != r.Turn || v.Containment != r.Containment || v.Epoch != r.Epoch {
		return store.StatusInvalid, nil, nil
	}
	epoch, err := nativeEpochCurrent(ctx, q, r, f[0])
	if err != nil {
		return 0, nil, err
	}
	if epoch != v.Epoch || v.State == "reconciled" {
		return store.StatusMissing, nil, nil
	}
	state, terminal, _, err := nativeOwnerLifecycle(ctx, q, f[0], nativeReservationSource(v))
	if err != nil {
		return 0, nil, err
	}
	if state == "running" {
		return store.StatusInvalid, nil, nil
	}
	if v.State == "finished" {
		if r.Expected != v.FinishedFrom || terminal != v.Terminal {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{terminal, epoch}, nil
	}
	if v.Claim != "" {
		ownerRequest := nativeReservationSource(v)
		owner, actor, _, _, err := nativeOwnerRead(ctx, q, ownerRequest)
		if err != nil {
			return 0, nil, err
		}
		if actor != f[0] || !nativeOwnerSame(owner, ownerRequest) {
			return store.StatusInvalid, nil, nil
		}
		actual, err := nativeOwnerCacheToken(ctx, q, ownerRequest, f[0])
		if err != nil {
			return 0, nil, err
		}
		phase, claim, turn, valid := nativeToken(actual)
		if !valid || claim != v.Claim || turn != v.Turn || actual != r.Expected || (phase != "complete" && phase != "invalid") {
			return store.StatusMissing, nil, nil
		}
	} else if r.Expected != "" {
		return store.StatusInvalid, nil, nil
	}
	sequence, err := strconv.ParseInt(terminal, 10, 64)
	if err != nil || sequence < 1 {
		return 0, nil, fmt.Errorf("invalid native finish terminal")
	}
	_, err = q.Exec(ctx, `UPDATE session_native_reservations SET state='finished',terminal_sequence=$5,finished_from=$6 WHERE session_id=$1 AND harness=$2 AND binding_sha256=$3 AND owner_id=$4`, r.Session, r.Harness, r.Binding, r.Owner, sequence, r.Expected)
	return store.StatusOK, []string{terminal, epoch}, err
}
