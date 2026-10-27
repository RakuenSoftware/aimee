package families

import (
	"context"
	wire "github.com/JBailes/aimee/server-go/aimee"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"strings"
	"testing"
)

func TestSessionPrimaryRequiresHostOwnerAndValidMetadata(t *testing.T) {
	for _, op := range []uint32{opServerSessionPrimaryGet, opServerSessionPrimarySet} {
		f := []string{"actor", "session"}
		if op == opServerSessionPrimarySet {
			f = append(f, "codex")
		}
		frame, e := wire.EncodeFields(op, f)
		if e != nil {
			t.Fatal(e)
		}
		db := newSessDB()
		_, s := Sessions.Handler(db)(ring.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if !Sessions.Ops[op].HostOnly || s != ring.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal(op, s)
		}
	}
	for _, raw := range []string{`null`, `[]`, `{"primary_agent":"a","primary_agent":"b"}`, `{"primary_agent":null}`, `{"primary_agent":1}`, `{"primary_agent":"` + strings.Repeat("x", 64) + `"}`} {
		db := newSessDB()
		db.row = []any{raw}
		s, _, e := sessionPrimaryGet(context.Background(), db, []string{"actor", "session"})
		if e != nil || s != store.StatusInvalid {
			t.Fatal(raw, s, e)
		}
	}
}
