package db1

import (
	"context"
	"encoding/binary"
	"errors"
	"strings"
	"testing"
	"time"
)

// fakeModule decodes a request exactly as src/modules/db1/module_adapter.c does
// and serves it from a map. Testing against a re-implementation of the C
// decoder is the point: if the Go encoder drifts from the wire contract, the
// frame stops parsing here rather than in production.
type fakeModule struct {
	state    map[string]string
	lastKey  string
	lastOp   uint32
	calls    int
	response []byte // when set, returned verbatim to test malformed handling
	err      error
}

func readField(body []byte, offset int) (string, int, bool) {
	if offset+4 > len(body) {
		return "", 0, false
	}
	n := int(binary.LittleEndian.Uint32(body[offset:]))
	offset += 4
	if n < 0 || offset+n > len(body) {
		return "", 0, false
	}
	return string(body[offset : offset+n]), offset + n, true
}

func (f *fakeModule) Call(_ context.Context, event, stage uint32, _ uint64,
	_ time.Duration, request []byte) ([]byte, error) {
	f.calls++
	if f.err != nil {
		return nil, f.err
	}
	if f.response != nil {
		return f.response, nil
	}
	if event != EventState || stage != StageState {
		return nil, errors.New("wrong event or stage")
	}
	if len(request) < 4 {
		return nil, errors.New("short request")
	}
	op := binary.LittleEndian.Uint32(request)
	key, offset, ok := readField(request, 4)
	if !ok {
		return nil, errors.New("bad key field")
	}
	payload, offset, ok := readField(request, offset)
	if !ok {
		return nil, errors.New("bad json field")
	}
	if offset != len(request) {
		return nil, errors.New("trailing bytes")
	}
	if key == "" || len(key) >= 512 || strings.Contains(key, "\x00") {
		return nil, errors.New("module would refuse this key")
	}
	f.lastKey, f.lastOp = key, op

	reply := func(status uint32, body string) []byte {
		out := make([]byte, 8+len(body))
		binary.LittleEndian.PutUint32(out, status)
		binary.LittleEndian.PutUint32(out[4:], uint32(len(body)))
		copy(out[8:], body)
		return out
	}
	switch op {
	case opStateLoad:
		if blob, ok := f.state[key]; ok && blob != "" {
			return reply(statusOK, blob), nil
		}
		return reply(statusMissing, ""), nil
	case opStateSave:
		if len(payload) >= StateMax {
			return reply(statusTooLong, ""), nil
		}
		if f.state == nil {
			f.state = map[string]string{}
		}
		f.state[key] = payload
		return reply(statusOK, ""), nil
	}
	return nil, errors.New("unknown op")
}

func newClient(t *testing.T, module StageCaller) *Client {
	t.Helper()
	client, err := NewClient(module, 0)
	if err != nil {
		t.Fatalf("NewClient: %v", err)
	}
	return client
}

func TestSaveThenLoadRoundTripsThroughTheWire(t *testing.T) {
	module := &fakeModule{}
	client := newClient(t, module)
	const key, blob = "gw:abc123", `{"turn":4,"frozen":true}`

	if err := client.SaveState(context.Background(), key, blob); err != nil {
		t.Fatalf("SaveState: %v", err)
	}
	if module.lastOp != opStateSave || module.lastKey != key {
		t.Fatalf("module saw op=%d key=%q", module.lastOp, module.lastKey)
	}
	got, ok, err := client.LoadState(context.Background(), key)
	if err != nil || !ok || got != blob {
		t.Fatalf("LoadState = (%q, %v, %v), want (%q, true, nil)", got, ok, err, blob)
	}
}

func TestLoadTreatsAMissAsAbsenceNotFailure(t *testing.T) {
	client := newClient(t, &fakeModule{})
	blob, ok, err := client.LoadState(context.Background(), "gw:cold")
	if err != nil {
		t.Fatalf("a first-turn miss must not be an error, got %v", err)
	}
	if ok || blob != "" {
		t.Fatalf("LoadState = (%q, %v), want empty and not found", blob, ok)
	}
}

func TestInvalidKeysAreRefusedWithoutCallingTheModule(t *testing.T) {
	for name, key := range map[string]string{
		"empty":     "",
		"embedded":  "gw:\x00truncated",
		"over-long": strings.Repeat("k", KeyMax+1),
	} {
		t.Run(name, func(t *testing.T) {
			module := &fakeModule{}
			client := newClient(t, module)
			if _, _, err := client.LoadState(context.Background(), key); !errors.Is(err, ErrInvalidKey) {
				t.Fatalf("LoadState err = %v, want ErrInvalidKey", err)
			}
			if err := client.SaveState(context.Background(), key, "x"); !errors.Is(err, ErrInvalidKey) {
				t.Fatalf("SaveState err = %v, want ErrInvalidKey", err)
			}
			if module.calls != 0 {
				t.Fatalf("a refused key still cost %d round trips", module.calls)
			}
		})
	}
}

func TestKeyAtTheLimitIsAcceptedAndOneOverIsNot(t *testing.T) {
	module := &fakeModule{}
	client := newClient(t, module)
	if err := client.SaveState(context.Background(), strings.Repeat("k", KeyMax), "v"); err != nil {
		t.Fatalf("a key at the documented limit must be accepted: %v", err)
	}
	if module.calls != 1 {
		t.Fatalf("calls = %d, want 1", module.calls)
	}
}

func TestOverLongStateIsRefusedBeforeTheRoundTrip(t *testing.T) {
	module := &fakeModule{}
	client := newClient(t, module)
	err := client.SaveState(context.Background(), "gw:k", strings.Repeat("s", StateMax))
	if !errors.Is(err, ErrStateTooLong) {
		t.Fatalf("SaveState err = %v, want ErrStateTooLong", err)
	}
	if module.calls != 0 {
		t.Fatalf("an over-long blob still cost %d round trips", module.calls)
	}
	// One under the cap is the largest the module accepts, so it must go through.
	if err := client.SaveState(context.Background(), "gw:k", strings.Repeat("s", StateMax-1)); err != nil {
		t.Fatalf("a blob one under the cap must be accepted: %v", err)
	}
}

func TestMalformedResponsesAreRefusedRatherThanRead(t *testing.T) {
	shortHeader := []byte{0, 0, 0}
	lying := make([]byte, 8+2)
	binary.LittleEndian.PutUint32(lying[4:], 99) // declares 99 bytes, carries 2
	for name, response := range map[string][]byte{
		"short header":   shortHeader,
		"length overrun": lying,
	} {
		t.Run(name, func(t *testing.T) {
			client := newClient(t, &fakeModule{response: response})
			if _, _, err := client.LoadState(context.Background(), "gw:k"); !errors.Is(err, ErrMalformed) {
				t.Fatalf("LoadState err = %v, want ErrMalformed", err)
			}
		})
	}
}

func TestUnexpectedStatusSurfacesAsAnError(t *testing.T) {
	response := make([]byte, 8)
	binary.LittleEndian.PutUint32(response, statusFailed)
	client := newClient(t, &fakeModule{response: response})

	_, _, err := client.LoadState(context.Background(), "gw:k")
	var status *StatusError
	if !errors.As(err, &status) || status.Status != statusFailed {
		t.Fatalf("LoadState err = %v, want a StatusError carrying %d", err, statusFailed)
	}
	if err := client.SaveState(context.Background(), "gw:k", "v"); !errors.As(err, &status) {
		t.Fatalf("SaveState err = %v, want a StatusError", err)
	}
}

func TestTransportFailuresPropagate(t *testing.T) {
	boom := errors.New("bus is down")
	client := newClient(t, &fakeModule{err: boom})
	if _, _, err := client.LoadState(context.Background(), "gw:k"); !errors.Is(err, boom) {
		t.Fatalf("LoadState err = %v, want the transport error", err)
	}
	if err := client.SaveState(context.Background(), "gw:k", "v"); !errors.Is(err, boom) {
		t.Fatalf("SaveState err = %v, want the transport error", err)
	}
}

func TestAnUnconfiguredClientRefuses(t *testing.T) {
	if _, err := NewClient(nil, time.Second); !errors.Is(err, ErrConfig) {
		t.Fatalf("NewClient(nil) err = %v, want ErrConfig", err)
	}
	var client *Client
	if _, _, err := client.LoadState(context.Background(), "gw:k"); !errors.Is(err, ErrConfig) {
		t.Fatalf("LoadState on a nil client err = %v, want ErrConfig", err)
	}
	if err := client.SaveState(context.Background(), "gw:k", "v"); !errors.Is(err, ErrConfig) {
		t.Fatalf("SaveState on a nil client err = %v, want ErrConfig", err)
	}
}

func TestEventAndStageMatchTheCarvedContract(t *testing.T) {
	// 4096 + ref*256 + stage, with DB1 at ref 30. If this changes, the C header
	// and the process contract have to change with it.
	if want := uint32(4096 + 30*256 + 1); EventState != want {
		t.Fatalf("EventState = %d, want %d", EventState, want)
	}
	if StageState != 1 {
		t.Fatalf("StageState = %d, want 1", StageState)
	}
}
