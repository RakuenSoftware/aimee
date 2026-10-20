package families

import (
	"context"
	"encoding/json"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
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
	for _, op := range []uint32{opSessionNativeOwnerClaim, opSessionNativeReconcile} {
		db := newSessDB()
		frame, err := wire.EncodeFields(op, nativeRecoveryFields(nativeRecoveryFixture()))
		if err != nil {
			t.Fatal(err)
		}
		_, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
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
