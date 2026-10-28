package config_test

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"

	contract "github.com/JBailes/aimee/server-go/config"
	moduleconfig "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"
	"github.com/RakuenSoftware/ouroboros-ring/go/ring"
)

type handlerCaller struct{ handler ring.ModuleHandler }
type failureCaller struct{ err error }

func (c handlerCaller) Call(_ context.Context, eventKind, stageID uint32, _ uint64,
	_ time.Duration, body []byte) ([]byte, error) {
	if eventKind != contract.EventConfig {
		return nil, errors.New("wrong event kind")
	}
	response, status := c.handler(ring.ModuleInvocation{StageID: stageID}, body)
	if status != ring.ModuleStatusOK {
		return nil, errors.New("module refused request")
	}
	return response, nil
}

func (c failureCaller) Call(context.Context, uint32, uint32, uint64, time.Duration,
	[]byte) ([]byte, error) {
	return nil, c.err
}

func newClient(t *testing.T) *contract.Client {
	t.Helper()
	store, err := moduleconfig.NewStore(filepath.Join(t.TempDir(), "aimee.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	client, err := contract.NewClient(handlerCaller{handler: moduleconfig.NewHandler(store)}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	return client
}

func TestClientRoundTripAndVersionConflict(t *testing.T) {
	client := newClient(t)
	if got := client.Int("autonomy.concurrency", -1); got != 5 {
		t.Fatalf("default concurrency = %d", got)
	}
	if err := client.Set("autonomy.concurrency", 7); err != nil {
		t.Fatal(err)
	}
	if got := client.Int("autonomy.concurrency", -1); got != 7 {
		t.Fatalf("updated concurrency = %d", got)
	}
	version, err := client.Version("trigger_rules")
	if err != nil || version == "" {
		t.Fatalf("version = %q, %v", version, err)
	}
	if err := client.SetVersioned("trigger_rules", []any{}, "stale"); err == nil {
		t.Fatal("stale version accepted")
	}
}

func TestClientPreservesTypedErrors(t *testing.T) {
	client := newClient(t)
	err := client.Set("not.editable", true)
	var moduleError *contract.ModuleError
	if !errors.As(err, &moduleError) || moduleError.Code != "invalid" {
		t.Fatalf("error = %#v", err)
	}
}

func TestClientClassifiesTransportFailuresAsUnavailable(t *testing.T) {
	transport := errors.New("capacity exhausted")
	client, err := contract.NewClient(failureCaller{err: transport}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	_, err = client.Values()
	if !errors.Is(err, contract.ErrUnavailable) || !errors.Is(err, transport) {
		t.Fatalf("transport error = %v", err)
	}
}

// lateCaller answers capability absent until it has been called `absent`
// times, then forwards to the real config handler: config starting after the
// module that asks it.
type lateCaller struct {
	absent int
	calls  *int
	inner  contract.StageCaller
}

func (c lateCaller) Call(ctx context.Context, eventKind, stageID uint32, trace uint64,
	deadline time.Duration, body []byte) ([]byte, error) {
	*c.calls++
	if *c.calls <= c.absent {
		return nil, ring.ErrModuleCallCapabilityAbsent
	}
	return c.inner.Call(ctx, eventKind, stageID, trace, deadline, body)
}

func TestSnapshotWhenServedWaitsOnlyForAbsence(t *testing.T) {
	store, err := moduleconfig.NewStore(filepath.Join(t.TempDir(), "aimee.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	calls := 0
	late, err := contract.NewClient(lateCaller{absent: 3, calls: &calls,
		inner: handlerCaller{handler: moduleconfig.NewHandler(store)}}, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	values, err := late.SnapshotWhenServed(context.Background(), 5*time.Second)
	if err != nil || values == nil || calls != 4 {
		t.Fatalf("a config module that starts late is waited for: calls=%d err=%v", calls, err)
	}

	calls = 0
	never, _ := contract.NewClient(lateCaller{absent: 1 << 30, calls: &calls}, time.Second)
	start := time.Now()
	if _, err := never.SnapshotWhenServed(context.Background(), 300*time.Millisecond); !errors.Is(err, ring.ErrModuleCallCapabilityAbsent) {
		t.Fatalf("absence past the wait is returned: %v", err)
	}
	if elapsed := time.Since(start); elapsed > 2*time.Second {
		t.Fatalf("the wait is bounded: %s", elapsed)
	}

	refused := errors.New("config refused")
	broken, _ := contract.NewClient(failureCaller{err: refused}, time.Second)
	start = time.Now()
	if _, err := broken.SnapshotWhenServed(context.Background(), 5*time.Second); !errors.Is(err, refused) {
		t.Fatalf("any other failure is returned: %v", err)
	}
	if elapsed := time.Since(start); elapsed > time.Second {
		t.Fatalf("a real failure is not retried: %s", elapsed)
	}

	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	if _, err := never.SnapshotWhenServed(ctx, 5*time.Second); !errors.Is(err, contract.ErrUnavailable) {
		t.Fatalf("a cancelled start stops waiting: %v", err)
	}
}
