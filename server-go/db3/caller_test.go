package db3

import (
	"context"
	"errors"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

// A scripted bus. The caller's job is fragmentation, correlation and deciding
// what a body means, and all three are testable without standing a bus up.
type scriptedWire struct {
	mu       sync.Mutex
	budget   uint32
	sent     [][]byte
	moreSeen []bool
	events   []bus.Event
	cancels  []uint64
	sendErr  error
	blockFor int
}

func (w *scriptedWire) InlineBudget() uint32 { return w.budget }
func (w *scriptedWire) HeartbeatNow()        {}

func (w *scriptedWire) RequestFragment(_ uint32, _ uint64, payload []byte, more bool) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	if w.blockFor > 0 {
		w.blockFor--
		return bus.ErrWouldBlock
	}
	if w.sendErr != nil {
		return w.sendErr
	}
	w.sent = append(w.sent, append([]byte(nil), payload...))
	w.moreSeen = append(w.moreSeen, more)
	return nil
}

func (w *scriptedWire) Cancel(_ uint32, correlation uint64) error {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.cancels = append(w.cancels, correlation)
	return nil
}

func (w *scriptedWire) Poll() (bus.Event, bool, error) {
	w.mu.Lock()
	defer w.mu.Unlock()
	if len(w.events) == 0 {
		return bus.Event{}, false, nil
	}
	event := w.events[0]
	w.events = w.events[1:]
	return event, true, nil
}

func (w *scriptedWire) push(events ...bus.Event) {
	w.mu.Lock()
	defer w.mu.Unlock()
	w.events = append(w.events, events...)
}

func (w *scriptedWire) sentBodies() [][]byte {
	w.mu.Lock()
	defer w.mu.Unlock()
	return append([][]byte(nil), w.sent...)
}

func (w *scriptedWire) cancelled() []uint64 {
	w.mu.Lock()
	defer w.mu.Unlock()
	return append([]uint64(nil), w.cancels...)
}

func replyEvent(correlation uint64, payload []byte, more bool) bus.Event {
	flags := bus.FReply
	if more {
		flags |= bus.FMore
	}
	return bus.Event{
		Frame:   bus.Frame{EventKind: EventSearch, CorrelationID: correlation, HdrFlags: flags},
		Payload: payload,
	}
}

func callerRequest() SearchRequest {
	return SearchRequest{
		RequestID: 5, RequiredGeneration: 3, Workspace: "w1", Project: "p1",
		RecordType: "memory", TopK: 2, Vector: []float32{1, 0, 0},
	}
}

func startCaller(t *testing.T, wire *scriptedWire) *SearchCaller {
	t.Helper()
	if wire.budget == 0 {
		wire.budget = 4096
	}
	ctx, cancel := context.WithCancel(context.Background())
	t.Cleanup(cancel)
	caller, err := newSearchCaller(ctx, wire)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(caller.Close)
	return caller
}

// answerWith replies to the first correlation the caller sends, once it has.
func answerWith(t *testing.T, wire *scriptedWire, build func(correlation uint64) []bus.Event) {
	t.Helper()
	go func() {
		deadline := time.Now().Add(2 * time.Second)
		for time.Now().Before(deadline) {
			if len(wire.sentBodies()) > 0 {
				wire.push(build(1)...)
				return
			}
			time.Sleep(time.Millisecond)
		}
	}()
}

func TestCallerReturnsAProvidersReply(t *testing.T) {
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	request := callerRequest()
	encoded, err := EncodeSearchReply(SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
		Candidates: []Candidate{{PointID: 41, Score: 0.9}},
	})
	if err != nil {
		t.Fatal(err)
	}
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{replyEvent(id, encoded, false)}
	})

	reply, failure, err := caller.Search(context.Background(), request)
	if err != nil || failure.Code != 0 {
		t.Fatalf("search returned (%+v, %v)", failure, err)
	}
	if len(reply.Candidates) != 1 || reply.Candidates[0].PointID != 41 {
		t.Fatalf("reply = %+v", reply)
	}
}

func TestCallerReassemblesAFragmentedReply(t *testing.T) {
	// A reply larger than the inline budget arrives in pieces, and a caller that
	// decoded the first piece would report a malformed provider for a perfectly
	// good answer.
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	request := callerRequest()
	encoded, err := EncodeSearchReply(SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
		Candidates: []Candidate{{PointID: 41, Score: 0.9}, {PointID: 42, Score: 0.5}},
	})
	if err != nil {
		t.Fatal(err)
	}
	split := len(encoded) / 2
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{
			replyEvent(id, encoded[:split], true),
			replyEvent(id, encoded[split:], false),
		}
	})

	reply, _, err := caller.Search(context.Background(), request)
	if err != nil {
		t.Fatal(err)
	}
	if len(reply.Candidates) != 2 {
		t.Fatalf("reassembled reply = %+v", reply)
	}
}

func TestCallerFragmentsARequestToTheInlineBudget(t *testing.T) {
	// The last fragment must say more=false, or the provider waits forever for a
	// continuation that never comes.
	wire := &scriptedWire{budget: 16}
	caller := startCaller(t, wire)
	request := callerRequest()
	encoded, _ := EncodeSearchReply(SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
	})
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{replyEvent(id, encoded, false)}
	})
	if _, _, err := caller.Search(context.Background(), request); err != nil {
		t.Fatal(err)
	}
	sent := wire.sentBodies()
	if len(sent) < 2 {
		t.Fatalf("a request larger than the budget was sent in %d fragment(s)", len(sent))
	}
	wire.mu.Lock()
	defer wire.mu.Unlock()
	for i, more := range wire.moreSeen {
		last := i == len(wire.moreSeen)-1
		if more == last {
			t.Fatalf("fragment %d had more=%v (last=%v)", i, more, last)
		}
	}
}

func TestCallerReportsATypedFailureRatherThanAnEmptyReply(t *testing.T) {
	// A provider's failure and an empty result are different facts. Reporting
	// the failure as an empty reply is how a broken provider becomes a corpus
	// with no matches.
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	request := callerRequest()
	encoded, err := EncodeSearchFailure(SearchFailure{
		RequestID: request.RequestID, Code: SearchFailureUnavailable,
	})
	if err != nil {
		t.Fatal(err)
	}
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{replyEvent(id, encoded, false)}
	})

	reply, failure, err := caller.Search(context.Background(), request)
	if err != nil {
		t.Fatal(err)
	}
	if failure.Code != SearchFailureUnavailable {
		t.Fatalf("failure = %+v", failure)
	}
	if len(reply.Candidates) != 0 {
		t.Errorf("a failure carried candidates: %+v", reply)
	}
}

func TestCallerRefusesABodyThatIsNeither(t *testing.T) {
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{replyEvent(id, []byte{0xde, 0xad, 0xbe, 0xef}, false)}
	})
	if _, _, err := caller.Search(context.Background(), callerRequest()); err == nil {
		t.Fatal("a body that is neither a reply nor a failure was accepted")
	}
}

func TestACancelledSearchTellsTheProviderToStop(t *testing.T) {
	// The provider is holding a store's resources for an answer nobody wants.
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	ctx, cancel := context.WithTimeout(context.Background(), 40*time.Millisecond)
	defer cancel()
	if _, _, err := caller.Search(ctx, callerRequest()); err == nil {
		t.Fatal("a search with no reply reported success")
	}
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) {
		if len(wire.cancelled()) > 0 {
			return
		}
		time.Sleep(time.Millisecond)
	}
	t.Error("a cancelled search sent no cancel to the provider")
}

func TestClosingFailsOutstandingSearchesRatherThanHangingThem(t *testing.T) {
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	done := make(chan error, 1)
	go func() {
		_, _, err := caller.Search(context.Background(), callerRequest())
		done <- err
	}()
	// Let the search register before closing.
	deadline := time.Now().Add(time.Second)
	for time.Now().Before(deadline) && len(wire.sentBodies()) == 0 {
		time.Sleep(time.Millisecond)
	}
	caller.Close()
	select {
	case err := <-done:
		if !errors.Is(err, ErrCallerClosed) {
			t.Fatalf("closing returned %v, want ErrCallerClosed", err)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("closing left a search hanging")
	}
}

func TestBackPressureIsRetriedAndOtherSendErrorsAreNot(t *testing.T) {
	// A full ring is back-pressure, not a failure. Treating it as one would drop
	// searches whenever the bus was busy.
	wire := &scriptedWire{blockFor: 3}
	caller := startCaller(t, wire)
	request := callerRequest()
	encoded, _ := EncodeSearchReply(SearchReply{
		RequestID: request.RequestID, Generation: request.RequiredGeneration,
	})
	answerWith(t, wire, func(id uint64) []bus.Event {
		return []bus.Event{replyEvent(id, encoded, false)}
	})
	if _, _, err := caller.Search(context.Background(), request); err != nil {
		t.Fatalf("back-pressure was reported as a failure: %v", err)
	}

	fatal := &scriptedWire{sendErr: errors.New("bus is gone")}
	fatalCaller := startCaller(t, fatal)
	if _, _, err := fatalCaller.Search(context.Background(), callerRequest()); err == nil {
		t.Fatal("a send error was retried instead of reported")
	}
}

func TestAMalformedRequestNeverReachesTheWire(t *testing.T) {
	wire := &scriptedWire{}
	caller := startCaller(t, wire)
	bad := callerRequest()
	bad.TopK = 0
	if _, _, err := caller.Search(context.Background(), bad); err == nil {
		t.Fatal("a request the contract refuses was sent")
	}
	if len(wire.sentBodies()) != 0 {
		t.Error("a malformed request was put on the wire")
	}
}
