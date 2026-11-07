package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	wire "github.com/JBailes/aimee/server-go/aimee"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"strings"
	"testing"
)

func nativeRecoveryFixture() nativeOwnerRequest {
	return nativeOwnerRequest{Version: 1, Session: "session", Turn: "old-turn", Harness: "codex", Scope: strings.Repeat("a", 64), Context: strings.Repeat("b", 64), Binding: strings.Repeat("c", 64), Owner: strings.Repeat("d", 32), Claim: strings.Repeat("e", 32), Containment: strings.Repeat("f", 64)}
}
func nativeRecoveryFields(r nativeOwnerRequest) []string {
	encoded, _ := json.Marshal(r)
	return []string{"actor", string(encoded)}
}
func TestSessionNativeRecoveryRequiresHostAndExactInput(t *testing.T) {
	for _, op := range []uint32{opSessionNativeOwnerClaim, opSessionNativeReconcile, opSessionNativeReserve, opSessionNativeReservationReconcile, opSessionNativeEpoch, opSessionNativeReservationLookup, opSessionNativeReservationFinish} {
		db := newSessDB()
		if !Sessions.Ops[op].HostOnly {
			t.Fatal("private resource operation lost host authority gate", op)
		}
		body := any(nativeRecoveryFixture())
		if op == opSessionNativeReconcile {
			r := nativeRecoveryFixture()
			r.Expected = r.token("pending")
			body = r
		}
		if op >= opSessionNativeReserve {
			r := nativeReservationRequest{Version: 1, Session: "session", Harness: "codex", Binding: strings.Repeat("a", 64)}
			if op != opSessionNativeEpoch {
				r.Owner = strings.Repeat("b", 32)
			}
			if op == opSessionNativeReserve || op == opSessionNativeReservationReconcile || op == opSessionNativeReservationFinish {
				r.Turn = "turn"
				r.Epoch = "0"
				descriptor := `{"version":1}`
				sum := sha256.Sum256([]byte(descriptor))
				r.Containment = hex.EncodeToString(sum[:])
				if op == opSessionNativeReserve {
					r.Descriptor = descriptor
				}
			}
			body = r
		}
		raw, _ := json.Marshal(body)
		frame, err := wire.EncodeFields(op, []string{"actor", string(raw)})
		if err != nil {
			t.Fatal(err)
		}
		_, status := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if status != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal("ordinary user invoked host recovery", status, db.executed)
		}
	}
	for _, change := range []func(*nativeOwnerRequest){func(r *nativeOwnerRequest) { r.Owner = "pid:12" }, func(r *nativeOwnerRequest) { r.Containment = "" }, func(r *nativeOwnerRequest) { r.Binding = "" }, func(r *nativeOwnerRequest) { r.Version = 0 }, func(r *nativeOwnerRequest) { r.Expected = "pending:pid:old-turn" }} {
		r := nativeRecoveryFixture()
		change(&r)
		db := newSessDB()
		status, _, err := sessionNativeReconcile(context.Background(), db, nativeRecoveryFields(r))
		if err != nil || status != store.StatusInvalid || len(db.executed) != 0 {
			t.Fatal("invalid authority input reached DB", status, err)
		}
	}
	r := nativeRecoveryFixture()
	r.Expected = r.token("pending")
	status, _, err := sessionNativeOwnerClaim(context.Background(), newSessDB(), nativeRecoveryFields(r))
	if err != nil || status != store.StatusInvalid {
		t.Fatal("claim overwrote unresolved owner", status, err)
	}
}

func TestSessionNativeActiveOwnedRequiresHostAndExactSession(t *testing.T) {
	if !Sessions.Ops[opSessionNativeActiveOwned].HostOnly {
		t.Fatal("active source discovery lost host gate")
	}
	for _, f := range [][]string{nil, {"actor"}, {"", "session"}, {"actor", ""}, {"actor", strings.Repeat("s", 129)}, {"actor", "session", "extra"}} {
		status, cells, err := sessionNativeActiveOwned(context.Background(), newSessDB(), f)
		if err != nil || status != store.StatusInvalid || len(cells) != 0 {
			t.Fatal(status, cells, err)
		}
	}
	db := newSessDB()
	frame, err := wire.EncodeFields(opSessionNativeActiveOwned, []string{"actor", "session"})
	if err != nil {
		t.Fatal(err)
	}
	_, status := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
	if status != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
		t.Fatal("untrusted discovery bypassed host admission", status, db.executed)
	}
}
