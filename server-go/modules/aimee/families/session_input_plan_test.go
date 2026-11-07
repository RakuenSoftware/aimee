package families

import (
	"context"
	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"strings"
	"testing"
)

func TestOriginalInputPlanActualHostAuthorization(t *testing.T) {
	for id, n := range map[uint32]int{62: 3, 63: 7, 64: 8, 65: 7} {
		spec := Sessions.Ops[id]
		if !spec.HostOnly || !spec.Tx || spec.Timeout != store.CanonicalHistoryTimeout {
			t.Fatal("wrong plan stage policy", id)
		}
		args := make([]string, n)
		for i := range args {
			args[i] = "body-owner-claim"
		}
		frame, _ := wire.EncodeFields(id, args)
		db := &sessDB{}
		_, status := Sessions.Handler(db)(bus.ModuleInvocation{StageID: StageSessions, PrincipalRef: 7}, frame)
		if status != bus.ModuleStatusInvalidRequest || len(db.executed) != 0 {
			t.Fatal("module body minted owner authority", id)
		}
	}
}
func TestOriginalInputPlanMalformedBeforeDatabase(t *testing.T) {
	good := []string{"s", "12345678-1234-4234-8234-123456789abc", "alice", "receiver", strings.Repeat("a", 64), deliveryHash("safe descriptor"), "safe descriptor"}
	for _, mutate := range []func([]string){func(a []string) { a[1] = "00000000-0000-0000-0000-000000000000" }, func(a []string) { a[4] = "bad" }, func(a []string) { a[5] = strings.Repeat("b", 64) }, func(a []string) { a[6] = strings.Repeat("x", 16385) }, func(a []string) { a[6] = "bad\x00" }} {
		a := append([]string{}, good...)
		mutate(a)
		status, _, err := sessionInputPlanReserve(context.Background(), nil, a)
		if err != nil || status != store.StatusInvalid {
			t.Fatal("malformed plan reached DB", status, err)
		}
	}
	for _, result := range []string{"", `{"a":1,"a":2}`, `{"a":"\u0000"}`} {
		status, _, err := sessionInputPlanTerminal(context.Background(), nil, []string{"1", "s", good[1], "alice", "receiver", good[5], result})
		if err != nil || status != store.StatusInvalid {
			t.Fatal("malformed terminal reached DB")
		}
	}
}
