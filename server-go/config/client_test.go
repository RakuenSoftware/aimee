package config_test

import (
	"context"
	"errors"
	"path/filepath"
	"testing"
	"time"

	contract "github.com/JBailes/aimee/server-go/config"
	"github.com/RakuenSoftware/aimee-module-config/server-go/bus"
	moduleconfig "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"
)

type handlerCaller struct{ handler bus.ModuleHandler }
type failureCaller struct{ err error }

func (c handlerCaller) Call(_ context.Context, eventKind, stageID uint32, _ uint64,
	_ time.Duration, body []byte) ([]byte, error) {
	if eventKind != contract.EventConfig {
		return nil, errors.New("wrong event kind")
	}
	response, status := c.handler(bus.ModuleInvocation{StageID: stageID}, body)
	if status != bus.ModuleStatusOK {
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
