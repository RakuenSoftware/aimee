package config_test

import (
	"context"
	"encoding/json"
	"path/filepath"
	"sync"
	"sync/atomic"
	"testing"
	"time"

	contract "github.com/JBailes/aimee/server-go/config"
	moduleconfig "github.com/RakuenSoftware/aimee-module-config/server-go/modules/config"
)

// countingCaller serves the real module handler and counts the calls that
// reached it. hold, when set, is run inside a call before it answers.
type countingCaller struct {
	handlerCaller
	calls atomic.Int64
	hold  func()
}

func (c *countingCaller) Call(ctx context.Context, eventKind, stageID uint32, trace uint64,
	deadline time.Duration, body []byte) ([]byte, error) {
	c.calls.Add(1)
	if c.hold != nil {
		c.hold()
	}
	return c.handlerCaller.Call(ctx, eventKind, stageID, trace, deadline, body)
}

// fakeSource is a NotificationSource the test drives directly.
type fakeSource struct {
	mu       sync.Mutex
	notify   map[uint32]func([]byte)
	overflow map[uint32]func()
}

func (f *fakeSource) OnNotification(kind uint32, fn func([]byte)) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.notify == nil {
		f.notify = map[uint32]func([]byte){}
	}
	f.notify[kind] = fn
}

func (f *fakeSource) OnOverflow(kind uint32, fn func()) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if f.overflow == nil {
		f.overflow = map[uint32]func(){}
	}
	f.overflow[kind] = fn
}

func (f *fakeSource) announce(version string) {
	payload, _ := json.Marshal(map[string]string{"version": version})
	f.notify[contract.EventConfigChanged](payload)
}

func cachingClient(t *testing.T) (*contract.Client, *countingCaller, *fakeSource) {
	t.Helper()
	store, err := moduleconfig.NewStore(filepath.Join(t.TempDir(), "aimee.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	caller := &countingCaller{handlerCaller: handlerCaller{handler: moduleconfig.NewHandler(store)}}
	client, err := contract.NewClient(caller, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	source := &fakeSource{}
	client.Subscribe(source)
	return client, caller, source
}

func TestAnUnsubscribedClientAsksEveryTime(t *testing.T) {
	store, err := moduleconfig.NewStore(filepath.Join(t.TempDir(), "aimee.yaml"))
	if err != nil {
		t.Fatal(err)
	}
	caller := &countingCaller{handlerCaller: handlerCaller{handler: moduleconfig.NewHandler(store)}}
	client, err := contract.NewClient(caller, time.Second)
	if err != nil {
		t.Fatal(err)
	}
	client.Int("autonomy.concurrency", -1)
	client.Int("autonomy.concurrency", -1)
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, want 2: without a subscription nothing is cached", got)
	}
}

func TestASubscribedClientAnswersRepeatReadsFromCache(t *testing.T) {
	client, caller, _ := cachingClient(t)
	for i := 0; i < 5; i++ {
		if got := client.Int("autonomy.concurrency", -1); got != 5 {
			t.Fatalf("concurrency = %d", got)
		}
	}
	// An unknown key is a typed refusal, and stays one without asking again.
	for i := 0; i < 3; i++ {
		if _, ok, err := client.IntValue("no.such.key"); ok || err != nil {
			t.Fatalf("unknown key = %v, %v", ok, err)
		}
	}
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, want one per distinct read", got)
	}
}

func TestAnAnnouncedVersionDropsTheCache(t *testing.T) {
	client, caller, source := cachingClient(t)
	changed := atomic.Int64{}
	client.OnChange(func() { changed.Add(1) })
	client.Int("autonomy.concurrency", -1)
	source.announce("v2")
	client.Int("autonomy.concurrency", -1)
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, want a fresh read after the change", got)
	}
	source.announce("v2") // a duplicate changes nothing
	client.Int("autonomy.concurrency", -1)
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, a duplicate announcement must not drop the cache", got)
	}
	if got := changed.Load(); got != 1 {
		t.Fatalf("OnChange ran %d times, want once", got)
	}
}

func TestAMissedAnnouncementDropsTheCache(t *testing.T) {
	client, caller, source := cachingClient(t)
	client.Int("autonomy.concurrency", -1)
	source.overflow[contract.EventConfigChanged]()
	client.Int("autonomy.concurrency", -1)
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, want a fresh read after an overflow", got)
	}
}

func TestTheClientsOwnWriteIsReadBackAtOnce(t *testing.T) {
	client, _, _ := cachingClient(t)
	if got := client.Int("autonomy.concurrency", -1); got != 5 {
		t.Fatalf("default = %d", got)
	}
	if err := client.Set("autonomy.concurrency", 7); err != nil {
		t.Fatal(err)
	}
	// Before any announcement arrives, the writer reads its own write.
	if got := client.Int("autonomy.concurrency", -1); got != 7 {
		t.Fatalf("after its own write the client read %d, want 7", got)
	}
}

func TestAReadThatRacedAChangeIsNotKept(t *testing.T) {
	client, caller, source := cachingClient(t)
	// The change is announced while the read is in flight: its reply may
	// predate the change, so it must not be cached.
	caller.hold = func() { source.announce("v-during") }
	client.Int("autonomy.concurrency", -1)
	caller.hold = nil
	client.Int("autonomy.concurrency", -1)
	if got := caller.calls.Load(); got != 2 {
		t.Fatalf("calls = %d, a reply that raced a change was kept", got)
	}
}

func TestEachCallerGetsItsOwnCopy(t *testing.T) {
	client, _, _ := cachingClient(t)
	first, err := client.Values()
	if err != nil {
		t.Fatal(err)
	}
	first["autonomy.concurrency"] = "tampered"
	second, err := client.Values()
	if err != nil {
		t.Fatal(err)
	}
	if second["autonomy.concurrency"] == "tampered" {
		t.Fatal("one caller's map changed what the next one read")
	}
}
