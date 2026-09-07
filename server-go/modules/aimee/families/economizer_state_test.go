package families

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"testing"
	"time"

	wire "github.com/JBailes/aimee/server-go/aimee"
	"github.com/JBailes/aimee/server-go/bus"
)

// memStore is the family's persistence without a database. It also counts
// writes per key, which is how the upsert test tells "replaced the row" from
// "appended another one".
type memStore struct {
	rows   map[string]string
	writes map[string]int
	failOn string
}

func newMemStore() *memStore {
	return &memStore{rows: map[string]string{}, writes: map[string]int{}}
}

func (m *memStore) load(_ context.Context, key string) (string, bool, error) {
	if m.failOn == "load" {
		return "", false, errors.New("load exploded")
	}
	value, ok := m.rows[key]
	return value, ok, nil
}

func (m *memStore) save(_ context.Context, key, blob string) error {
	if m.failOn == "save" {
		return errors.New("save exploded")
	}
	m.rows[key] = blob
	m.writes[key]++
	return nil
}

// handlerCaller seats the module handler behind the StageCaller interface the
// shipped client expects, so a test can drive the real client against the real
// handler with no bus in between.
type handlerCaller struct {
	handler bus.ModuleHandler
	t       *testing.T
}

func (h handlerCaller) Call(_ context.Context, event, stage uint32, _ uint64,
	_ time.Duration, frame []byte) ([]byte, error) {
	h.t.Helper()
	if event != EventState {
		h.t.Fatalf("client called event %d, module serves %d", event, EventState)
	}
	response, status := h.handler(bus.ModuleInvocation{StageID: stage}, frame)
	if status != bus.ModuleStatusOK {
		// The bus reports a non-OK module status as a transport failure, so the
		// client never sees a payload for one.
		return nil, fmt.Errorf("module status %v", status)
	}
	return response, nil
}

func clientAgainst(t *testing.T, s blobStore) *wire.Client {
	t.Helper()
	client, err := wire.NewClient(handlerCaller{handler: newHandler(s), t: t}, time.Second)
	if err != nil {
		t.Fatalf("new client: %v", err)
	}
	return client
}

// The point of this test: the shipped client and this module must agree on the
// bytes. Kind 11777 is bound to one serving slot, so the swap from the C module
// to this one is invisible to callers only if the frames match exactly.
func TestClientRoundTripAgainstNativeModule(t *testing.T) {
	client := clientAgainst(t, newMemStore())
	ctx := context.Background()

	if _, found, err := client.LoadState(ctx, "conv-1"); err != nil || found {
		t.Fatalf("cold load = (found %v, err %v), want (false, nil)", found, err)
	}
	if err := client.SaveState(ctx, "conv-1", `{"page":1}`); err != nil {
		t.Fatalf("save: %v", err)
	}
	state, found, err := client.LoadState(ctx, "conv-1")
	if err != nil || !found || state != `{"page":1}` {
		t.Fatalf("load = (%q, %v, %v)", state, found, err)
	}
}

// The C kept one row per session with DELETE-then-INSERT; here the primary key
// does it and the write is a single upsert. What callers must observe is
// unchanged: a second save replaces rather than accumulates.
func TestSaveReplacesRatherThanAccumulates(t *testing.T) {
	backing := newMemStore()
	client := clientAgainst(t, backing)
	ctx := context.Background()

	for _, blob := range []string{`{"page":1}`, `{"page":2}`, `{"page":3}`} {
		if err := client.SaveState(ctx, "conv-1", blob); err != nil {
			t.Fatalf("save %s: %v", blob, err)
		}
	}
	state, found, err := client.LoadState(ctx, "conv-1")
	if err != nil || !found || state != `{"page":3}` {
		t.Fatalf("load = (%q, %v, %v), want the newest blob", state, found, err)
	}
	if got := len(backing.rows); got != 1 {
		t.Fatalf("rows for one session = %d, want 1", got)
	}
	if got := backing.writes["conv-1"]; got != 3 {
		t.Fatalf("writes = %d, want 3 -- the test did not exercise replacement", got)
	}
}

// A stored blob at or over the wire cap is refused, not truncated: truncated
// JSON does not parse, and a caller treating a short read as "no state" would
// silently lose the conversation's page table.
func TestOverLongStoredStateIsRefusedNotTruncated(t *testing.T) {
	backing := newMemStore()
	backing.rows["conv-1"] = strings.Repeat("x", StateMax)
	client := clientAgainst(t, backing)

	state, found, err := client.LoadState(context.Background(), "conv-1")
	if err == nil {
		t.Fatalf("load = (%q, %v, nil), want a refusal", state, found)
	}
	var statusErr *wire.StatusError
	if !errors.As(err, &statusErr) || statusErr.Status != statusTooLong {
		t.Fatalf("err = %v, want status %d (too_long)", err, statusTooLong)
	}
}

func TestSaveRefusesOverLongPayload(t *testing.T) {
	backing := newMemStore()
	handler := newHandler(backing)
	// Past the client, which caps first -- the module may not trust a frame just
	// because the shipped client would not have sent it.
	frame := encodeFrame(opStateSave, "conv-1", strings.Repeat("x", StateMax))
	response, status := handler(bus.ModuleInvocation{StageID: StageState}, frame)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if got := decodeStatus(t, response); got != statusTooLong {
		t.Fatalf("status = %d, want %d (too_long)", got, statusTooLong)
	}
	if len(backing.rows) != 0 {
		t.Fatalf("an over-long blob was stored anyway")
	}
}

func TestStoreFailureIsReportedInBandNotAsTransportError(t *testing.T) {
	for _, test := range []struct {
		name string
		fail string
		op   uint32
	}{
		{"load", "load", opStateLoad},
		{"save", "save", opStateSave},
	} {
		t.Run(test.name, func(t *testing.T) {
			backing := newMemStore()
			backing.failOn = test.fail
			handler := newHandler(backing)
			response, status := handler(bus.ModuleInvocation{StageID: StageState},
				encodeFrame(test.op, "conv-1", ""))
			if status != bus.ModuleStatusOK {
				t.Fatalf("status = %v, want OK with an in-band failure", status)
			}
			if got := decodeStatus(t, response); got != statusFailed {
				t.Fatalf("status = %d, want %d (failed)", got, statusFailed)
			}
		})
	}
}

// A bad key is a well-formed question with an answer, so it is reported in-band.
// A malformed FRAME is not a question at all, so it is a bus-level refusal.
func TestBadKeyIsInBandButMalformedFrameIsInvalidRequest(t *testing.T) {
	handler := newHandler(newMemStore())

	response, status := handler(bus.ModuleInvocation{StageID: StageState},
		encodeFrame(opStateLoad, "", ""))
	if status != bus.ModuleStatusOK {
		t.Fatalf("empty key: status = %v, want OK", status)
	}
	if got := decodeStatus(t, response); got != statusInvalid {
		t.Fatalf("empty key: status = %d, want %d (invalid)", got, statusInvalid)
	}

	response, status = handler(bus.ModuleInvocation{StageID: StageState},
		encodeFrame(opStateLoad, "conv\x00truncated", ""))
	if status != bus.ModuleStatusOK || decodeStatus(t, response) != statusInvalid {
		t.Fatalf("NUL key: status = %v / %d, want OK / invalid", status, decodeStatus(t, response))
	}

	for _, name := range []string{"short", "key-length-lies", "payload-length-lies", "trailing-bytes"} {
		t.Run(name, func(t *testing.T) {
			if _, status := handler(bus.ModuleInvocation{StageID: StageState},
				malformedFrame(name)); status != bus.ModuleStatusInvalidRequest {
				t.Fatalf("status = %v, want InvalidRequest", status)
			}
		})
	}
}

func TestUnknownOpAndWrongStageAreRefused(t *testing.T) {
	handler := newHandler(newMemStore())
	if _, status := handler(bus.ModuleInvocation{StageID: StageState},
		encodeFrame(99, "conv-1", "")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("unknown op: status = %v, want InvalidRequest", status)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageState + 1},
		encodeFrame(opStateLoad, "conv-1", "")); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("wrong stage: status = %v, want InvalidRequest", status)
	}
}
