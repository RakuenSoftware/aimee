package families

import (
	"context"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
	"os"
	"os/signal"
	"syscall"
	"testing"
	"time"
)

func TestOwnedPlanServer(t *testing.T) {
	db, _ := deliveryFixture(t)
	ctx, cancel := signal.NotifyContext(context.Background(), syscall.SIGTERM, syscall.SIGINT)
	defer cancel()
	handler := Sessions.Handler(db)
	wrapped := func(i ring.ModuleInvocation, f []byte) ([]byte, ring.ModuleStatus) {
		b, s := handler(i, f)
		op, a, ok := store.DecodeRequest(f)
		if os.Getenv("OWNED_INPUT_DROP_AFTER_COMMIT") == "1" && ok && op == 64 && len(a) == 8 && a[0] == "broker-plan-lost" && s == ring.ModuleStatusOK {
			status, _, valid := store.DecodeRequest(b)
			if valid && status == store.StatusOK {
				os.Exit(0)
			}
		}
		return b, s
	}
	if e := ring.RunModuleProcess(ctx, ring.ModuleProcessConfig{SocketPath: os.Getenv("OWNED_INPUT_BUS_SOCKET"), ModuleName: "owned-input-plan", PrincipalClass: 1, PrincipalRef: 30, Stages: []ring.ModuleStage{{EventKind: 11782, StageID: StageSessions}}, Handler: wrapped, MaxIdlePollInterval: time.Millisecond}); e != nil {
		t.Fatal(e)
	}
}
func TestOwnedPlanBrokerSetup(t *testing.T) {
	_, p := deliveryFixture(t)
	_, e := p.Exec(context.Background(), `INSERT INTO server_sessions(id,principal) VALUES('broker-plan','alice'),('broker-plan-lost','alice'),('broker-terminal','alice')`)
	if e != nil {
		t.Fatal(e)
	}
}
