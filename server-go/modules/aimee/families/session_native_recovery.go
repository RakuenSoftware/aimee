package families

// These HostOnly operations are owner primitives, not a claim that a process
// exited. Production recovery must not call reconciliation until a separately
// qualified, retained whole-process-tree boundary has proved quiescence.
import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strconv"
	"strings"
)

const opSessionNativeOwnerClaim uint32 = 38
const opSessionNativeReconcile uint32 = 39

type nativeOwnerRequest struct {
	Version     int    `json:"version"`
	Session     string `json:"session_id"`
	Turn        string `json:"turn_id"`
	Harness     string `json:"harness"`
	Scope       string `json:"scope_sha256"`
	Context     string `json:"context_sha256"`
	Binding     string `json:"binding_sha256"`
	Owner       string `json:"owner_id"`
	Claim       string `json:"claim_id"`
	Containment string `json:"containment_sha256"`
	Expected    string `json:"expected_token"`
}

func nativeHex(s string, n int) bool { return len(s) == n && strings.Trim(s, "0123456789abcdef") == "" }
func nativeToken(token string) (string, string, string, bool) {
	p := strings.SplitN(token, ":", 3)
	if len(p) != 3 || !nativeHex(p[1], 32) || p[2] == "" || len(p[2]) > 128 {
		return "", "", "", false
	}
	if p[0] != "pending" && p[0] != "finishing" && p[0] != "complete" && p[0] != "invalid" {
		return "", "", "", false
	}
	return p[0], p[1], p[2], true
}
func nativeOwnerInput(f []string) (nativeOwnerRequest, bool) {
	var r nativeOwnerRequest
	if len(f) != 2 || f[0] == "" || len(f[0]) > 128 || len(f[1]) > 4096 || !canonical.Decode([]byte(f[1]), &r) {
		return r, false
	}
	valid := r.Version == 1 && r.Session != "" && len(r.Session) <= 128 && r.Turn != "" && len(r.Turn) <= 128 && (r.Harness == "codex" || r.Harness == "claude") && nativeHex(r.Scope, 64) && nativeHex(r.Context, 64) && nativeHex(r.Binding, 64) && nativeHex(r.Owner, 32) && nativeHex(r.Claim, 32) && nativeHex(r.Containment, 64)
	return r, valid
}
func (r nativeOwnerRequest) key() string {
	sum := sha256.Sum256([]byte(r.Harness + ":" + r.Scope + ":" + r.Context))
	return r.Harness + "-ir-v1-" + hex.EncodeToString(sum[:]) + ":cache"
}
func (r nativeOwnerRequest) token(phase string) string { return phase + ":" + r.Claim + ":" + r.Turn }

const nativeRecoveryFenceSQL = `SELECT 1 FROM session_native_recovery_fences WHERE session_id=$1 AND turn_id=$2`

func nativeRecoveryFenced(ctx context.Context, q store.Queryer, session, turn string) (bool, error) {
	var one int
	err := q.QueryRow(ctx, nativeRecoveryFenceSQL, session, turn).Scan(&one)
	if store.IsNoRows(err) {
		return false, nil
	}
	return err == nil, err
}
func nativeOwnerLock(ctx context.Context, q store.Queryer, actor, session string) (uint32, error) {
	var owner string
	err := q.QueryRow(ctx, sessionIRLockSQL, session).Scan(&owner)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil
	}
	if err != nil {
		return 0, err
	}
	if owner != actor {
		return store.StatusInvalid, nil
	}
	return store.StatusOK, nil
}

// Source inspection is exact-turn and bounded. Duplicate/malformed lifecycle or
// foreign observations refuse; neither a new capture nor a model response can
// synthesize the old execution's state.
func nativeOwnerLifecycle(ctx context.Context, q store.Queryer, actor string, r nativeOwnerRequest) (string, string, string, error) {
	var foreign bool
	if err := q.QueryRow(ctx, `SELECT EXISTS(SELECT 1 FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND actor<>$3)`, r.Session, r.Turn, actor).Scan(&foreign); err != nil {
		return "", "", "", err
	}
	if foreign {
		return "", "", "", fmt.Errorf("native recovery source actor mismatch")
	}
	rows, err := q.Query(ctx, `SELECT sequence::text,event_id,actor,payload FROM session_ir_events WHERE session_id=$1 AND turn_id=$2 AND payload::jsonb->>'kind'='execution' ORDER BY sequence LIMIT 3`, r.Session, r.Turn)
	if err != nil {
		return "", "", "", err
	}
	defer rows.Close()
	state, sequence, eventID := "", "", ""
	for rows.Next() {
		var actorAtSource, raw string
		if err := rows.Scan(&sequence, &eventID, &actorAtSource, &raw); err != nil {
			return "", "", "", err
		}
		var event struct {
			Kind    string                     `json:"kind"`
			Payload canonical.ExecutionPayload `json:"payload"`
		}
		if actorAtSource != actor || !canonical.ValidSessionEvent([]byte(raw)) || !canonical.Decode([]byte(raw), &event) || event.Kind != "execution" {
			return "", "", "", fmt.Errorf("invalid native recovery lifecycle")
		}
		next := event.Payload.Status
		if state == "" {
			if next != "running" {
				return "", "", "", fmt.Errorf("native recovery requires admitted source")
			}
		} else if state != "running" || next == "running" {
			return "", "", "", fmt.Errorf("conflicting native recovery lifecycle")
		}
		state = next
	}
	if err := rows.Err(); err != nil {
		return "", "", "", err
	}
	if state == "" {
		return "", "", "", fmt.Errorf("native recovery source missing")
	}
	return state, sequence, eventID, nil
}

const nativeOwnerReadSQL = `SELECT actor,turn_id,harness,scope_sha256,context_sha256,binding_sha256,owner_id,claim_id,containment_sha256,recovered_from,coalesce(terminal_sequence,0)::text FROM session_native_cache_owners WHERE session_id=$1 AND cache_key=$2`

func nativeOwnerRead(ctx context.Context, q store.Queryer, r nativeOwnerRequest) (nativeOwnerRequest, string, string, string, error) {
	var actual nativeOwnerRequest
	var actor, recovered, terminal string
	err := q.QueryRow(ctx, nativeOwnerReadSQL, r.Session, r.key()).Scan(&actor, &actual.Turn, &actual.Harness, &actual.Scope, &actual.Context, &actual.Binding, &actual.Owner, &actual.Claim, &actual.Containment, &recovered, &terminal)
	actual.Version = 1
	actual.Session = r.Session
	return actual, actor, recovered, terminal, err
}
func nativeOwnerSame(a, b nativeOwnerRequest) bool { a.Expected = ""; b.Expected = ""; return a == b }
func nativeOwnerCacheToken(ctx context.Context, q store.Queryer, r nativeOwnerRequest, actor string) (string, error) {
	var actual string
	err := q.QueryRow(ctx, sessionHarnessGetSQL, r.Session, r.key(), actor).Scan(&actual)
	if store.IsNoRows(err) {
		return "", nil
	}
	return actual, err
}
func sessionNativeOwnerClaim(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, valid := nativeOwnerInput(f)
	if !valid {
		return store.StatusInvalid, nil, nil
	}
	if r.Expected != "" {
		phase, _, _, ok := nativeToken(r.Expected)
		if !ok || (phase != "complete" && phase != "invalid") {
			return store.StatusInvalid, nil, nil
		}
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	fenced, err := nativeRecoveryFenced(ctx, q, r.Session, r.Turn)
	if err != nil {
		return 0, nil, err
	}
	if fenced {
		return store.StatusInvalid, nil, nil
	}
	state, _, _, err := nativeOwnerLifecycle(ctx, q, f[0], r)
	if err != nil {
		return 0, nil, err
	}
	if state != "running" {
		return store.StatusInvalid, nil, nil
	}
	actual, err := nativeOwnerCacheToken(ctx, q, r, f[0])
	if err != nil {
		return 0, nil, err
	}
	if actual == r.token("pending") {
		owner, actor, recovered, _, err := nativeOwnerRead(ctx, q, r)
		if err != nil && !store.IsNoRows(err) {
			return 0, nil, err
		}
		if err == nil && actor == f[0] && recovered == "" && nativeOwnerSame(owner, r) {
			return store.StatusOK, nil, nil
		}
		return store.StatusMissing, nil, nil
	}
	if actual != r.Expected {
		return store.StatusMissing, nil, nil
	}
	if _, err := q.Exec(ctx, sessionHarnessBindSQL, r.Session, r.key(), r.token("pending")); err != nil {
		return 0, nil, err
	}
	_, err = q.Exec(ctx, `INSERT INTO session_native_cache_owners(session_id,cache_key,actor,turn_id,harness,scope_sha256,context_sha256,binding_sha256,owner_id,claim_id,containment_sha256) VALUES($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11) ON CONFLICT(session_id,cache_key) DO UPDATE SET actor=excluded.actor,turn_id=excluded.turn_id,harness=excluded.harness,scope_sha256=excluded.scope_sha256,context_sha256=excluded.context_sha256,binding_sha256=excluded.binding_sha256,owner_id=excluded.owner_id,claim_id=excluded.claim_id,containment_sha256=excluded.containment_sha256,recovered_from='',terminal_sequence=NULL`, r.Session, r.key(), f[0], r.Turn, r.Harness, r.Scope, r.Context, r.Binding, r.Owner, r.Claim, r.Containment)
	return store.StatusOK, nil, err
}
func sessionNativeReconcile(ctx context.Context, q store.Queryer, f []string) (uint32, []string, error) {
	r, valid := nativeOwnerInput(f)
	phase, claim, turn, tokenValid := nativeToken(r.Expected)
	if !valid || !tokenValid || (phase != "pending" && phase != "finishing") || claim != r.Claim || turn != r.Turn {
		return store.StatusInvalid, nil, nil
	}
	status, err := nativeOwnerLock(ctx, q, f[0], r.Session)
	if err != nil || status != store.StatusOK {
		return status, nil, err
	}
	owner, actor, recovered, storedTerminal, err := nativeOwnerRead(ctx, q, r)
	if store.IsNoRows(err) {
		return store.StatusMissing, nil, nil
	}
	if err != nil {
		return 0, nil, err
	}
	if actor != f[0] || !nativeOwnerSame(owner, r) {
		return store.StatusInvalid, nil, nil
	}
	actual, err := nativeOwnerCacheToken(ctx, q, r, f[0])
	if err != nil {
		return 0, nil, err
	}
	if recovered != "" {
		if recovered != r.Expected || actual != r.token("invalid") {
			return store.StatusMissing, nil, nil
		}
		var fencedActor, fencedOwner, fencedClaim, fencedContainment, fencedTerminal string
		err := q.QueryRow(ctx, `SELECT actor,owner_id,claim_id,containment_sha256,terminal_sequence::text FROM session_native_recovery_fences WHERE session_id=$1 AND turn_id=$2`, r.Session, r.Turn).Scan(&fencedActor, &fencedOwner, &fencedClaim, &fencedContainment, &fencedTerminal)
		if store.IsNoRows(err) {
			return store.StatusMissing, nil, nil
		}
		if err != nil {
			return 0, nil, err
		}
		if fencedActor != f[0] || fencedOwner != r.Owner || fencedClaim != r.Claim || fencedContainment != r.Containment || fencedTerminal != storedTerminal {
			return store.StatusInvalid, nil, nil
		}
		state, terminal, _, err := nativeOwnerLifecycle(ctx, q, f[0], r)
		if err != nil {
			return 0, nil, err
		}
		if state == "running" || terminal != storedTerminal {
			return store.StatusInvalid, nil, nil
		}
		return store.StatusOK, []string{storedTerminal}, nil
	}
	if actual != r.Expected {
		return store.StatusMissing, nil, nil
	}
	fenced, err := nativeRecoveryFenced(ctx, q, r.Session, r.Turn)
	if err != nil {
		return 0, nil, err
	}
	if fenced {
		return store.StatusInvalid, nil, nil
	}
	state, terminal, _, err := nativeOwnerLifecycle(ctx, q, f[0], r)
	if err != nil {
		return 0, nil, err
	}
	if state == "running" {
		raw := `{"kind":"execution","payload":{"status":"cancelled","error":"native execution interrupted by server restart"}}`
		eventID := "native-recovery:" + r.Owner + ":" + r.Claim
		status, cells, err := sessionIRAppendInternal(ctx, q, []string{r.Session, eventID, r.Turn, f[0], raw}, false)
		if err != nil || status != store.StatusOK || len(cells) != 1 {
			return status, nil, err
		}
		terminal = cells[0]
	}
	sequence, err := strconv.ParseInt(terminal, 10, 64)
	if err != nil || sequence < 1 {
		return 0, nil, fmt.Errorf("invalid native recovery terminal reference")
	}
	if _, err = q.Exec(ctx, `INSERT INTO session_native_recovery_fences(session_id,turn_id,actor,owner_id,claim_id,containment_sha256,terminal_sequence) VALUES($1,$2,$3,$4,$5,$6,$7)`, r.Session, r.Turn, f[0], r.Owner, r.Claim, r.Containment, sequence); err != nil {
		return 0, nil, err
	}
	// The session row lock serializes every ordinary append and harness CAS. The
	// equality predicate still prevents any stale recovery overwriting a new owner.
	tag, err := q.Exec(ctx, `UPDATE session_harness_handles SET native_id=$4 WHERE session_id=$1 AND harness=$2 AND native_id=$3`, r.Session, r.key(), r.Expected, r.token("invalid"))
	if err != nil {
		return 0, nil, err
	}
	if tag.RowsAffected() != 1 {
		return store.StatusMissing, nil, nil
	}
	if _, err = q.Exec(ctx, `UPDATE session_native_cache_owners SET recovered_from=$3,terminal_sequence=$4 WHERE session_id=$1 AND cache_key=$2`, r.Session, r.key(), r.Expected, sequence); err != nil {
		return 0, nil, err
	}
	return store.StatusOK, []string{terminal}, nil
}
