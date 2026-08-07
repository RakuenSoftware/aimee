package bus

import (
	"bytes"
	"context"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"

	"golang.org/x/sys/unix"
)

type fakeModuleBus struct {
	mu        sync.Mutex
	input     []Event
	replies   []Event
	budget    uint32
	heartbeat uint64
}

func (f *fakeModuleBus) Poll() (Event, bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.input) == 0 {
		return Event{}, false, nil
	}
	event := f.input[0]
	f.input = f.input[1:]
	return event, true, nil
}

func (f *fakeModuleBus) ReplyFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	flags := uint16(FReply)
	if more {
		flags |= FMore
	}
	f.replies = append(f.replies, Event{Frame: Frame{HdrFlags: flags, EventKind: kind,
		CorrelationID: correlation}, Payload: append([]byte(nil), payload...)})
	return nil
}

func (f *fakeModuleBus) Heartbeat(now uint64)       { f.heartbeat = now }
func (f *fakeModuleBus) EpochChanged() bool         { return false }
func (f *fakeModuleBus) moduleInlineBudget() uint32 { return f.budget }

func moduleRequestEvent(t *testing.T, kind uint32, correlation uint64, message ModuleMessage,
	body []byte, more bool) Event {
	t.Helper()
	payload := make([]byte, ModuleMessageHeaderLen+len(body))
	message.BodyLen = uint32(len(body))
	if _, err := message.Encode(payload); err != nil {
		t.Fatal(err)
	}
	copy(payload[ModuleMessageHeaderLen:], body)
	flags := uint16(FRequest)
	if more {
		flags |= FMore
	}
	return Event{Frame: Frame{HdrFlags: flags, EventKind: kind, CorrelationID: correlation},
		Payload: payload}
}

func waitModuleReplies(t *testing.T, fake *fakeModuleBus, count int) []Event {
	t.Helper()
	deadline := time.Now().Add(2 * time.Second)
	for time.Now().Before(deadline) {
		fake.mu.Lock()
		if len(fake.replies) >= count {
			result := append([]Event(nil), fake.replies...)
			fake.mu.Unlock()
			return result
		}
		fake.mu.Unlock()
		time.Sleep(time.Millisecond)
	}
	t.Fatalf("timed out waiting for %d module replies", count)
	return nil
}

func TestGoModuleRuntimeFragmentedRoundTrip(t *testing.T) {
	const kind uint32 = 5889
	request := bytes.Repeat([]byte("go-module-"), 19)
	message := ModuleMessage{Operation: ModuleOpInvoke, StageID: 1, TraceID: 77}
	fake := &fakeModuleBus{budget: 72}
	fake.input = []Event{
		moduleRequestEvent(t, kind, 9, message, request[:80], true),
		moduleRequestEvent(t, kind, 9, message, request[80:], false),
	}
	config := ModuleProcessConfig{SocketPath: "/unused", ModuleName: "go-test",
		PrincipalClass: 1, PrincipalRef: 7, Stages: []ModuleStage{{EventKind: kind, StageID: 1}},
		Handler: func(invocation ModuleInvocation, body []byte) ([]byte, ModuleStatus) {
			if invocation.StageID != 1 || invocation.TraceID != 77 || !bytes.Equal(body, request) {
				return nil, ModuleStatusInvalidRequest
			}
			return append([]byte(nil), body...), ModuleStatusOK
		}}
	stages, err := validateModuleConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runModuleClient(ctx, config, stages, fake) }()
	wantFragments := (len(request) + int(fake.budget) - ModuleMessageHeaderLen - 1) /
		(int(fake.budget) - ModuleMessageHeaderLen)
	replies := waitModuleReplies(t, fake, wantFragments)
	var response []byte
	for i, event := range replies {
		message, err := DecodeModuleMessage(event.Payload)
		if err != nil || message.Operation != ModuleOpResult || message.Status != ModuleStatusOK ||
			message.StageID != 1 || message.TraceID != 77 {
			t.Fatalf("reply %d: %#v, %v", i, message, err)
		}
		response = append(response, event.Payload[ModuleMessageHeaderLen:]...)
		if i+1 < len(replies) && event.Frame.HdrFlags&FMore == 0 {
			t.Fatalf("reply %d ended fragmented response early", i)
		}
	}
	if replies[len(replies)-1].Frame.HdrFlags&FMore != 0 || !bytes.Equal(response, request) {
		t.Fatal("fragmented Go module response mismatch")
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestGoModuleRuntimeCancellationAndCapabilityAbsent(t *testing.T) {
	const kind uint32 = 6401
	message := ModuleMessage{Operation: ModuleOpInvoke, StageID: 1, TraceID: 91}
	fake := &fakeModuleBus{budget: 128}
	fake.input = []Event{
		moduleRequestEvent(t, kind, 12, message, []byte("wait"), false),
		{Frame: Frame{HdrFlags: FCancel, EventKind: kind, CorrelationID: 12}},
	}
	config := ModuleProcessConfig{SocketPath: "/unused", ModuleName: "go-cancel",
		PrincipalClass: 1, PrincipalRef: 9, Stages: []ModuleStage{{EventKind: kind, StageID: 1}},
		Handler: func(invocation ModuleInvocation, _ []byte) ([]byte, ModuleStatus) {
			for !invocation.Cancelled() {
				time.Sleep(time.Millisecond)
			}
			return []byte("must be dropped"), ModuleStatusOK
		}}
	stages, err := validateModuleConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runModuleClient(ctx, config, stages, fake) }()
	replies := waitModuleReplies(t, fake, 1)
	reply, err := DecodeModuleMessage(replies[0].Payload)
	if err != nil || reply.Status != ModuleStatusCancelled || reply.BodyLen != 0 {
		t.Fatalf("cancel reply = %#v, %v", reply, err)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}

	absent := &fakeModuleBus{budget: 128, input: []Event{
		moduleRequestEvent(t, kind, 13, message, nil, false),
	}}
	config.Handler = nil
	ctx, cancel = context.WithCancel(context.Background())
	done = make(chan error, 1)
	go func() { done <- runModuleClient(ctx, config, stages, absent) }()
	replies = waitModuleReplies(t, absent, 1)
	reply, err = DecodeModuleMessage(replies[0].Payload)
	if err != nil || reply.Status != ModuleStatusCapabilityAbsent || reply.BodyLen != 0 {
		t.Fatalf("absent reply = %#v, %v", reply, err)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestGoModuleRuntimeConfigAndBodyLimitMatchC(t *testing.T) {
	if ModuleMessageMaxBody != 16*1024*1024 {
		t.Fatalf("Go max body = %d, want C contract 16777216", ModuleMessageMaxBody)
	}
	_, err := validateModuleConfig(ModuleProcessConfig{SocketPath: "/bus", ModuleName: "bad",
		PrincipalClass: 1, PrincipalRef: 1,
		Stages: []ModuleStage{{EventKind: 1, StageID: 1}, {EventKind: 1, StageID: 2}}})
	if err == nil {
		t.Fatal("accepted duplicate event kind")
	}
}

func TestConnectModuleWaitsOutStaleSocket(t *testing.T) {
	path := t.TempDir() + "/stale.sock"
	fd, err := unix.Socket(unix.AF_UNIX, unix.SOCK_SEQPACKET|unix.SOCK_CLOEXEC, 0)
	if err != nil {
		t.Fatal(err)
	}
	if err := unix.Bind(fd, &unix.SockaddrUnix{Name: path}); err != nil {
		unix.Close(fd)
		t.Fatal(err)
	}
	unix.Close(fd) // Leave a pathname with no listener, as a restarted host can.

	ctx, cancel := context.WithTimeout(context.Background(), 25*time.Millisecond)
	defer cancel()
	_, err = connectModule(ctx, ModuleProcessConfig{SocketPath: path})
	if !errors.Is(err, context.DeadlineExceeded) {
		t.Fatalf("stale socket attach = %v, want context deadline", err)
	}
}

func TestConnectModuleDoesNotHideMissingSocket(t *testing.T) {
	ctx, cancel := context.WithTimeout(context.Background(), time.Second)
	defer cancel()
	_, err := connectModule(ctx, ModuleProcessConfig{SocketPath: t.TempDir() + "/missing.sock"})
	if !errors.Is(err, unix.ENOENT) {
		t.Fatalf("missing socket attach = %v, want ENOENT", err)
	}
}

func TestStandaloneModuleInvocationUsesDeadlineWithoutSyntheticCancellation(t *testing.T) {
	if (ModuleInvocation{StageID: 1}).Cancelled() {
		t.Fatal("standalone invocation was treated as cancelled")
	}
	if !(ModuleInvocation{StageID: 1, DeadlineNS: 1}).Cancelled() {
		t.Fatal("expired standalone invocation was not cancelled")
	}
}

// A failing handler's reason is logged before the non-OK reply drops it, so the
// rendering has to survive whatever a handler returns -- including a truncated
// or non-UTF8 body.
func TestModuleDetailRendersAnyHandlerBody(t *testing.T) {
	if got := moduleDetail(nil); got != "no detail" {
		t.Fatalf("empty body rendered %q", got)
	}
	if got := moduleDetail([]byte("workdir does not exist")); got != "workdir does not exist" {
		t.Fatalf("short body rendered %q", got)
	}
	long := moduleDetail([]byte(strings.Repeat("x", 400)))
	if len(long) != 303 || !strings.HasSuffix(long, "...") {
		t.Fatalf("long body rendered %d chars: %q", len(long), long)
	}
	if got := moduleDetail([]byte{'o', 'k', 0xff}); got != "ok" {
		t.Fatalf("invalid utf8 rendered %q", got)
	}
}
