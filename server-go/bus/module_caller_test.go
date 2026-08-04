package bus

import (
	"context"
	"errors"
	"strings"
	"sync"
	"testing"
	"time"
)

// fakeCallerBus stands in for an attached client. It records what the caller
// sent and answers with whatever the test scripts, so the invoke framing,
// correlation and reassembly are exercised without a live host.
type fakeCallerBus struct {
	mu          sync.Mutex
	sent        []Event
	pending     []Event
	cancelled   []uint64
	budget      uint32
	replyTo     func(f *fakeCallerBus, kind uint32, correlation uint64, body []byte)
	requestFail error
}

func (f *fakeCallerBus) Poll() (Event, bool, error) {
	f.mu.Lock()
	defer f.mu.Unlock()
	if len(f.pending) == 0 {
		return Event{}, false, nil
	}
	event := f.pending[0]
	f.pending = f.pending[1:]
	return event, true, nil
}

func (f *fakeCallerBus) RequestFragment(kind uint32, correlation uint64, payload []byte, more bool) error {
	if f.requestFail != nil {
		return f.requestFail
	}
	f.mu.Lock()
	flags := uint16(FRequest)
	if more {
		flags |= FMore
	}
	f.sent = append(f.sent, Event{Frame: Frame{HdrFlags: flags, EventKind: kind,
		CorrelationID: correlation}, Payload: append([]byte(nil), payload...)})
	complete := !more
	var assembled []byte
	if complete {
		for _, event := range f.sent {
			if event.Frame.CorrelationID != correlation {
				continue
			}
			message, err := DecodeModuleMessage(event.Payload)
			if err != nil {
				continue
			}
			assembled = append(assembled,
				event.Payload[ModuleMessageHeaderLen:ModuleMessageHeaderLen+int(message.BodyLen)]...)
		}
	}
	f.mu.Unlock()
	if complete && f.replyTo != nil {
		f.replyTo(f, kind, correlation, assembled)
	}
	return nil
}

func (f *fakeCallerBus) Cancel(kind uint32, correlation uint64) error {
	f.mu.Lock()
	defer f.mu.Unlock()
	f.cancelled = append(f.cancelled, correlation)
	return nil
}

func (f *fakeCallerBus) moduleInlineBudget() uint32 { return f.budget }

// queueResult enqueues a ModuleOpResult the way a module's reply arrives:
// correlated, fragmented across the same inline budget.
func (f *fakeCallerBus) queueResult(kind uint32, correlation uint64, status ModuleStatus,
	body []byte, stageID uint32) {
	f.mu.Lock()
	defer f.mu.Unlock()
	chunk := int(f.budget) - ModuleMessageHeaderLen
	first := true
	for offset := 0; first || offset < len(body); {
		first = false
		part := len(body) - offset
		if part > chunk {
			part = chunk
		}
		more := offset+part < len(body)
		payload := make([]byte, ModuleMessageHeaderLen+part)
		message := ModuleMessage{Operation: ModuleOpResult, Status: status, StageID: stageID,
			BodyLen: uint32(part)}
		if _, err := message.Encode(payload); err != nil {
			panic(err)
		}
		copy(payload[ModuleMessageHeaderLen:], body[offset:offset+part])
		flags := uint16(FReply)
		if more {
			flags |= FMore
		}
		f.pending = append(f.pending, Event{Frame: Frame{HdrFlags: flags, EventKind: kind,
			CorrelationID: correlation}, Payload: payload})
		offset += part
	}
}

func echoingBus(budget uint32) *fakeCallerBus {
	return &fakeCallerBus{budget: budget,
		replyTo: func(f *fakeCallerBus, kind uint32, correlation uint64, body []byte) {
			f.queueResult(kind, correlation, ModuleStatusOK, body, 1)
		}}
}

// A body larger than one inline frame has to survive fragmentation in both
// directions. This is the case that silently truncates a review artifact if the
// reassembly is wrong.
func TestModuleCallerRoundTripsAFragmentedBody(t *testing.T) {
	fake := echoingBus(128)
	caller := newModuleCaller(fake)
	body := []byte(strings.Repeat("roundtable artifact ", 200))
	got, err := caller.Call(context.Background(), 9474, 2, 77, time.Second, body)
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != string(body) {
		t.Fatalf("round trip changed the body: got %d bytes, want %d", len(got), len(body))
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if len(fake.sent) < 2 {
		t.Fatalf("a body larger than the inline budget was sent in %d frame(s)", len(fake.sent))
	}
	for _, event := range fake.sent {
		message, err := DecodeModuleMessage(event.Payload)
		if err != nil {
			t.Fatal(err)
		}
		if message.Operation != ModuleOpInvoke || message.StageID != 2 || message.TraceID != 77 {
			t.Fatalf("invoke framing lost: %+v", message)
		}
	}
}

// The reply is correlated, so anything else on the client belongs to another
// call and must not be mistaken for this one's result.
func TestModuleCallerIgnoresOtherCorrelationsAndKinds(t *testing.T) {
	fake := &fakeCallerBus{budget: 128,
		replyTo: func(f *fakeCallerBus, kind uint32, correlation uint64, body []byte) {
			f.queueResult(kind, correlation+1, ModuleStatusOK, []byte("someone else"), 1)
			f.queueResult(kind+1, correlation, ModuleStatusOK, []byte("another kind"), 1)
			f.queueResult(kind, correlation, ModuleStatusOK, []byte("mine"), 1)
		}}
	caller := newModuleCaller(fake)
	got, err := caller.Call(context.Background(), 9474, 2, 1, time.Second, []byte("ask"))
	if err != nil {
		t.Fatal(err)
	}
	if string(got) != "mine" {
		t.Fatalf("caller accepted another call's reply: %q", got)
	}
}

// A module's own failure must surface as that status, not as a generic error:
// the caller decides differently for an invalid request than for an absent
// capability.
func TestModuleCallerSurfacesModuleStatus(t *testing.T) {
	fake := &fakeCallerBus{budget: 128,
		replyTo: func(f *fakeCallerBus, kind uint32, correlation uint64, body []byte) {
			f.queueResult(kind, correlation, ModuleStatusInvalidRequest, nil, 1)
		}}
	_, err := newModuleCaller(fake).Call(context.Background(), 9474, 2, 1, time.Second, []byte("ask"))
	var status *ModuleCallStatusError
	if !errors.As(err, &status) || status.Status != ModuleStatusInvalidRequest {
		t.Fatalf("err = %v, want an invalid-request status error", err)
	}
	if !errors.Is(err, ErrModuleCallFailed) {
		t.Fatal("status error does not unwrap to ErrModuleCallFailed")
	}
}

// A module that never answers must not hang the caller, and must be told to
// stop -- otherwise it keeps spending on work nobody is waiting for.
func TestModuleCallerDeadlineCancelsTheRequest(t *testing.T) {
	fake := &fakeCallerBus{budget: 128} // never replies
	start := time.Now()
	_, err := newModuleCaller(fake).Call(context.Background(), 9474, 2, 1, 80*time.Millisecond,
		[]byte("ask"))
	if !errors.Is(err, ErrModuleCallDeadline) {
		t.Fatalf("err = %v, want a deadline", err)
	}
	if elapsed := time.Since(start); elapsed > 3*time.Second {
		t.Fatalf("caller waited %s past its own deadline", elapsed)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if len(fake.cancelled) != 1 {
		t.Fatalf("expired call sent %d cancels; the module would keep working", len(fake.cancelled))
	}
}

func TestModuleCallerHonoursContextCancellation(t *testing.T) {
	fake := &fakeCallerBus{budget: 128}
	ctx, cancel := context.WithCancel(context.Background())
	cancel()
	_, err := newModuleCaller(fake).Call(ctx, 9474, 2, 1, time.Minute, []byte("ask"))
	if !errors.Is(err, ErrModuleCallCancelled) {
		t.Fatalf("err = %v, want cancellation", err)
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	if len(fake.cancelled) != 1 {
		t.Fatal("cancelled call left the module running")
	}
}

// Each call must key its own correlation, or a slow reply from a previous call
// would be handed to the next one as its result.
func TestModuleCallerUsesADistinctCorrelationPerCall(t *testing.T) {
	fake := echoingBus(128)
	caller := newModuleCaller(fake)
	for i := 0; i < 3; i++ {
		if _, err := caller.Call(context.Background(), 9474, 2, 1, time.Second, []byte("ask")); err != nil {
			t.Fatal(err)
		}
	}
	fake.mu.Lock()
	defer fake.mu.Unlock()
	seen := map[uint64]bool{}
	for _, event := range fake.sent {
		seen[event.Frame.CorrelationID] = true
	}
	if len(seen) != 3 {
		t.Fatalf("three calls used %d correlation(s)", len(seen))
	}
}
