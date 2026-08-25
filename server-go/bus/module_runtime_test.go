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

func TestModuleInvocationRemaining(t *testing.T) {
	const limit = time.Second
	if got := (ModuleInvocation{}).Remaining(limit); got != limit {
		t.Fatalf("no deadline remaining = %v, want %v", got, limit)
	}
	if got := (ModuleInvocation{DeadlineNS: 1}).Remaining(limit); got != 0 {
		t.Fatalf("expired deadline remaining = %v, want 0", got)
	}
	now := monotonicNowNS()
	if now == 0 {
		t.Skip("CLOCK_MONOTONIC unavailable")
	}
	got := (ModuleInvocation{DeadlineNS: now + uint64(25*time.Millisecond)}).Remaining(limit)
	if got <= 0 || got > 25*time.Millisecond {
		t.Fatalf("bounded remaining = %v, want (0, 25ms]", got)
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

func TestTheModuleLoopHandsOverEventsThatAreNotItsWork(t *testing.T) {
	// A module that CALLS something -- postgres searching a vector database --
	// cannot poll for its own reply: this loop is the single reader on the
	// client, and a second reader would race it and silently eat events meant
	// for the other. So the loop reads everything and hands over what is not a
	// request for one of its stages.
	const kind uint32 = 5889
	const replyKind uint32 = 0x80030004
	message := ModuleMessage{Operation: ModuleOpInvoke, StageID: 1, TraceID: 5}
	fake := &fakeModuleBus{budget: 512}
	fake.input = []Event{
		{Frame: Frame{HdrFlags: FReply, EventKind: replyKind, CorrelationID: 31},
			Payload: []byte("search-reply")},
		{Frame: Frame{HdrFlags: FNotification, EventKind: replyKind, CorrelationID: 32},
			Payload: []byte("applied")},
		moduleRequestEvent(t, kind, 9, message, []byte("work"), false),
	}

	absorbed := make(chan Event, 4)
	config := ModuleProcessConfig{SocketPath: "/unused", ModuleName: "go-test",
		PrincipalClass: 1, PrincipalRef: 7, Stages: []ModuleStage{{EventKind: kind, StageID: 1}},
		Absorb: func(event Event) { absorbed <- event },
		Handler: func(_ ModuleInvocation, body []byte) ([]byte, ModuleStatus) {
			return append([]byte(nil), body...), ModuleStatusOK
		}}
	stages, err := validateModuleConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runModuleClient(ctx, config, stages, fake) }()

	for _, want := range []string{"search-reply", "applied"} {
		select {
		case event := <-absorbed:
			if string(event.Payload) != want {
				t.Fatalf("absorbed %q, want %q", event.Payload, want)
			}
		case <-time.After(2 * time.Second):
			t.Fatalf("the loop never handed over %q; a caller would wait forever "+
				"for a reply this loop already consumed", want)
		}
	}

	// And the stage still ran: absorbing does not cost the module its own work.
	replies := waitModuleReplies(t, fake, 1)
	if body := replies[0].Payload[ModuleMessageHeaderLen:]; string(body) != "work" {
		t.Fatalf("stage reply carried %q", body)
	}
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}

func TestAModuleWithNoAbsorbDropsThoseEventsRatherThanStalling(t *testing.T) {
	// Most modules never call out. A nil Absorb must not become a condition the
	// loop has to handle at runtime.
	const kind uint32 = 5889
	message := ModuleMessage{Operation: ModuleOpInvoke, StageID: 1, TraceID: 5}
	fake := &fakeModuleBus{budget: 512}
	fake.input = []Event{
		{Frame: Frame{HdrFlags: FReply, EventKind: 0x80030004, CorrelationID: 31},
			Payload: []byte("nobody-asked")},
		moduleRequestEvent(t, kind, 9, message, []byte("work"), false),
	}
	config := ModuleProcessConfig{SocketPath: "/unused", ModuleName: "go-test",
		PrincipalClass: 1, PrincipalRef: 7, Stages: []ModuleStage{{EventKind: kind, StageID: 1}},
		Handler: func(_ ModuleInvocation, body []byte) ([]byte, ModuleStatus) {
			return append([]byte(nil), body...), ModuleStatusOK
		}}
	stages, err := validateModuleConfig(config)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	done := make(chan error, 1)
	go func() { done <- runModuleClient(ctx, config, stages, fake) }()
	waitModuleReplies(t, fake, 1)
	cancel()
	if err := <-done; err != nil {
		t.Fatal(err)
	}
}
