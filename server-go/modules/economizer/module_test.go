package economizer

import (
	"context"
	"encoding/binary"
	"encoding/json"
	"errors"
	"os"
	"path/filepath"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func invoke(t *testing.T, req ReduceRequest) (ReduceResponse, bus.ModuleStatus) {
	t.Helper()
	body, err := json.Marshal(req)
	if err != nil {
		t.Fatal(err)
	}
	out, status := NewHandler()(bus.ModuleInvocation{StageID: StageReduce}, body)
	var resp ReduceResponse
	if status == bus.ModuleStatusOK {
		if err := json.Unmarshal(out, &resp); err != nil {
			t.Fatalf("response is not JSON: %v", err)
		}
	}
	return resp, status
}

// memStore is a StateStore that keeps blobs in memory, so the state tests
// exercise the module's own load/save path without needing a bus.
type memStore struct {
	blobs   map[string]string
	loadErr error
	saveErr error
	loads   int
	saves   int
}

func (m *memStore) LoadState(_ context.Context, key string) (string, bool, error) {
	m.loads++
	if m.loadErr != nil {
		return "", false, m.loadErr
	}
	blob, ok := m.blobs[key]
	return blob, ok && blob != "", nil
}

func (m *memStore) SaveState(_ context.Context, key, blob string) error {
	m.saves++
	if m.saveErr != nil {
		return m.saveErr
	}
	if m.blobs == nil {
		m.blobs = map[string]string{}
	}
	m.blobs[key] = blob
	return nil
}

func invokeWithStore(t *testing.T, store StateStore, req ReduceRequest) (ReduceResponse, bus.ModuleStatus) {
	t.Helper()
	body, err := json.Marshal(req)
	if err != nil {
		t.Fatal(err)
	}
	out, status := NewHandlerWithStore(store)(bus.ModuleInvocation{StageID: StageReduce}, body)
	var resp ReduceResponse
	if status == bus.ModuleStatusOK {
		if err := json.Unmarshal(out, &resp); err != nil {
			t.Fatalf("response is not JSON: %v", err)
		}
	}
	return resp, status
}

func rawMessages(t *testing.T, n int) json.RawMessage {
	t.Helper()
	arr := makeMessages(n)
	return json.RawMessage(PrintJSONUnformatted(arr))
}

func TestModuleReduceStage(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		SystemPrompt:  "sys",
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if !resp.Mutated || resp.Reason != "reduced" || len(resp.Messages) == 0 {
		t.Fatalf("expected a reduction: %+v", resp)
	}
	if resp.ReducedTokens >= resp.BaselineTokens || resp.RemovedTokens == 0 {
		t.Errorf("ledger shows no saving: %+v", resp)
	}
	// The emitted array must be valid and folded.
	got := ParseJSON(string(resp.Messages))
	if got == nil || !got.IsArray() {
		t.Fatal("emitted messages are not a JSON array")
	}
	if !strings.Contains(got.At(0).GetString("content"), "folded") {
		t.Error("the first message should be the fold summary")
	}
}

// A response with no messages field means "forward your ORIGINAL untouched" —
// the caller must not be handed a re-serialized copy it might send instead.
func TestModuleNoMutationOmitsMessages(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:    rawMessages(t, 20),
		Seam:        "delegate",
		HistoryFold: true,
		MeasureOnly: true,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if resp.Mutated || len(resp.Messages) != 0 {
		t.Errorf("measure-only must not emit messages: %+v", resp)
	}
	if resp.Reason != "measured" || resp.BaselineTokens == 0 {
		t.Errorf("measure-only should still report the ledger: %+v", resp)
	}
}

// State round-trips through the STORE, so a conversation keeps its freeze
// boundary and page table across turns without the caller ever handling it.
func TestModuleStateRoundTrip(t *testing.T) {
	store := &memStore{}
	turn := func() ReduceRequest {
		return ReduceRequest{
			Messages:      rawMessages(t, 20),
			Seam:          "delegate",
			HistoryFold:   true,
			ClosetEnabled: true,
			RecallEnabled: true,
			StateKey:      "conv-1",
		}
	}
	if _, status := invokeWithStore(t, store, turn()); status != bus.ModuleStatusOK {
		t.Fatalf("first turn status = %v", status)
	}
	first, ok := store.blobs["conv-1"]
	if !ok || first == "" {
		t.Fatalf("the module must persist its own state, store = %+v", store.blobs)
	}

	if _, status := invokeWithStore(t, store, turn()); status != bus.ModuleStatusOK {
		t.Fatalf("second turn status = %v", status)
	}
	if store.blobs["conv-1"] == "" {
		t.Error("state should survive a second turn")
	}
	// The turn counter is the module's to advance now that it owns the blob.
	var restored ReduceState
	if err := RestoreState(&restored, store.blobs["conv-1"]); err != nil {
		t.Fatalf("stored state is unreadable: %v", err)
	}
	if restored.Turn < 1 {
		t.Errorf("turn should advance across turns, got %d", restored.Turn)
	}
}

// A conversation with no key still reduces; it just never warms up. This is the
// path a caller takes when it has no conversation identity to name.
func TestModuleWithoutAStateKeyNeverPersists(t *testing.T) {
	store := &memStore{}
	resp, status := invokeWithStore(t, store, ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
	})
	if status != bus.ModuleStatusOK || !resp.Mutated {
		t.Fatalf("the reduction should still run: status=%v resp=%+v", status, resp)
	}
	if store.loads != 0 || store.saves != 0 {
		t.Errorf("an unkeyed request touched the store: %d loads, %d saves",
			store.loads, store.saves)
	}
}

// An unreachable store costs one cold turn, not the reduction. Reducer state is
// an optimization, so every storage failure lands in the same safe place.
func TestModuleStoreFailuresAreNotFatal(t *testing.T) {
	for name, store := range map[string]*memStore{
		"load fails": {loadErr: errStoreDown},
		"save fails": {saveErr: errStoreDown},
	} {
		t.Run(name, func(t *testing.T) {
			resp, status := invokeWithStore(t, store, ReduceRequest{
				Messages:      rawMessages(t, 20),
				Seam:          "delegate",
				HistoryFold:   true,
				ClosetEnabled: true,
				StateKey:      "conv-1",
			})
			if status != bus.ModuleStatusOK {
				t.Fatalf("a store failure must not fail the reduction: %v", status)
			}
			if !resp.Mutated {
				t.Error("the reduction should still have run")
			}
		})
	}
}

// A module wired without a store is the degradation that never heals: it is
// decided once at startup and lasts the life of the process. It is therefore
// the one that most needs counting.
func TestAMissingStoreIsCountedWhenAConversationWasNamed(t *testing.T) {
	handler := NewHandlerWithStore(nil)
	body, err := json.Marshal(ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "gateway",
		HistoryFold:   true,
		ClosetEnabled: true,
		StateKey:      "conv-1",
	})
	if err != nil {
		t.Fatal(err)
	}
	if _, status := handler(bus.ModuleInvocation{StageID: StageReduce}, body); status != bus.ModuleStatusOK {
		t.Fatalf("a storeless module must still reduce: %v", status)
	}
	out, status := handler(bus.ModuleInvocation{StageID: StageStats}, []byte(`{}`))
	if status != bus.ModuleStatusOK {
		t.Fatalf("stats stage status = %v", status)
	}
	var raw map[string]any
	if err := json.Unmarshal(out, &raw); err != nil {
		t.Fatalf("stats are not JSON: %v", err)
	}
	if n, _ := raw["economizer_state_unavailable"].(float64); n < 1 {
		t.Errorf("a named conversation with no store went uncounted: %v", raw)
	}
}

// A store that degrades must be COUNTED, because degrading is silent by design:
// the fold keeps running and simply stops warming up, which looks exactly like a
// fold that is working.
func TestStoreDegradationIsCounted(t *testing.T) {
	for name, tc := range map[string]struct {
		store   *memStore
		counter string
	}{
		"unreachable on load": {&memStore{loadErr: errStoreDown}, "economizer_state_unavailable"},
		"unreadable blob":     {&memStore{blobs: map[string]string{"conv-1": "{not valid"}}, "economizer_state_unavailable"},
		"write fails":         {&memStore{saveErr: errStoreDown}, "economizer_state_save_failed"},
	} {
		t.Run(name, func(t *testing.T) {
			handler := NewHandlerWithStore(tc.store)
			body, err := json.Marshal(ReduceRequest{
				Messages:      rawMessages(t, 20),
				Seam:          "gateway",
				HistoryFold:   true,
				ClosetEnabled: true,
				StateKey:      "conv-1",
			})
			if err != nil {
				t.Fatal(err)
			}
			if _, status := handler(bus.ModuleInvocation{StageID: StageReduce}, body); status != bus.ModuleStatusOK {
				t.Fatalf("degradation must not fail the reduction: %v", status)
			}
			out, status := handler(bus.ModuleInvocation{StageID: StageStats}, []byte(`{}`))
			if status != bus.ModuleStatusOK {
				t.Fatalf("stats stage status = %v", status)
			}
			var raw map[string]any
			if err := json.Unmarshal(out, &raw); err != nil {
				t.Fatalf("stats are not JSON: %v", err)
			}
			got, ok := raw[tc.counter]
			if !ok {
				t.Fatalf("counter %q absent from stats: %v", tc.counter, raw)
			}
			if n, _ := got.(float64); n < 1 {
				t.Errorf("counter %q = %v, want at least 1", tc.counter, got)
			}
		})
	}
}

// An unreadable state is DISCARDED, not fatal: the reduction still runs, it just
// starts cold. Failing the request would cost more than one turn of warmth.
func TestModuleBadStateIsNotFatal(t *testing.T) {
	store := &memStore{blobs: map[string]string{"conv-1": "{not valid"}}
	resp, status := invokeWithStore(t, store, ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
		StateKey:      "conv-1",
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("a bad state blob must not fail the request: %v", status)
	}
	if !resp.Mutated {
		t.Error("the reduction should still have run")
	}
}

func TestModuleRejectsBadRequests(t *testing.T) {
	h := NewHandler()
	// Wrong stage.
	if _, st := h(bus.ModuleInvocation{StageID: 99}, []byte(`{}`)); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("unknown stage: %v", st)
	}
	// Not JSON.
	if _, st := h(bus.ModuleInvocation{StageID: StageReduce}, []byte(`nope`)); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("bad body: %v", st)
	}
	// Messages that are not an array.
	body, _ := json.Marshal(ReduceRequest{Messages: json.RawMessage(`{"a":1}`), Seam: "delegate"})
	if _, st := h(bus.ModuleInvocation{StageID: StageReduce}, body); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("non-array messages: %v", st)
	}
	// An unknown seam is refused rather than defaulted, so a caller typo cannot
	// silently reduce at the wrong seam.
	body, _ = json.Marshal(ReduceRequest{Messages: rawMessages(t, 4), Seam: "elsewhere"})
	if _, st := h(bus.ModuleInvocation{StageID: StageReduce}, body); st != bus.ModuleStatusInvalidRequest {
		t.Errorf("unknown seam: %v", st)
	}
}

func TestModuleJSONCompactStagePreservesBytes(t *testing.T) {
	h := NewHandler()
	source := []byte(" \n { \"a\" : [ 1.2300e+02, \" x \\u0061 \" ] } \r\n")
	out, status := h(bus.ModuleInvocation{StageID: StageJSONCompact}, source)
	if status != bus.ModuleStatusOK || len(out) < auxHeaderLen {
		t.Fatalf("status=%v response=%x", status, out)
	}
	if binary.LittleEndian.Uint32(out[0:4]) != compactMagic ||
		binary.LittleEndian.Uint16(out[6:8]) != uint16(JSONOK) {
		t.Fatalf("bad response header: %x", out[:auxHeaderLen])
	}
	payloadLen := int(binary.LittleEndian.Uint32(out[8:12]))
	if payloadLen != len(out)-auxHeaderLen ||
		string(out[auxHeaderLen:]) != `{"a":[1.2300e+02," x \u0061 "]}` {
		t.Fatalf("compacted payload = %q", out[auxHeaderLen:])
	}

	invalidUTF8 := []byte{'[', ' ', '"', 0xc0, 0x80, '"', ' ', ']'}
	out, status = h(bus.ModuleInvocation{StageID: StageJSONCompact}, invalidUTF8)
	if status != bus.ModuleStatusOK ||
		binary.LittleEndian.Uint16(out[6:8]) != uint16(JSONInvalidUTF8) {
		t.Fatalf("invalid UTF-8 response=%x status=%v", out, status)
	}
}

func TestModuleJSONCompactStageRefusals(t *testing.T) {
	h := NewHandler()
	cases := []struct {
		name string
		in   []byte
		want JSONResult
	}{
		{"not shorter", []byte(`{"a":1}`), JSONNotShorter},
		{"duplicate key", []byte(`{ "a": 1, "a": 2 }`), JSONDuplicateKey},
		{"too deep", []byte(strings.Repeat("[", JSONMaxDepth+1) + strings.Repeat("]", JSONMaxDepth+1)), JSONTooDeep},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			out, status := h(bus.ModuleInvocation{StageID: StageJSONCompact}, tc.in)
			if status != bus.ModuleStatusOK || len(out) < auxHeaderLen ||
				JSONResult(binary.LittleEndian.Uint16(out[6:8])) != tc.want {
				t.Fatalf("response=%x status=%v want=%v", out, status, tc.want)
			}
		})
	}
	oversize := make([]byte, JSONMaxInput+1)
	out, status := h(bus.ModuleInvocation{StageID: StageJSONCompact}, oversize)
	if status != bus.ModuleStatusOK || JSONResult(binary.LittleEndian.Uint16(out[6:8])) != JSONTooLarge {
		t.Fatalf("oversize response=%x status=%v", out, status)
	}
}

func recallRequest(dir, ref string) []byte {
	out := make([]byte, recallRequestLen+len(dir)+len(ref))
	binary.LittleEndian.PutUint32(out[0:4], recallMagic)
	binary.LittleEndian.PutUint16(out[4:6], auxWireVersion)
	binary.LittleEndian.PutUint32(out[8:12], uint32(len(dir)))
	binary.LittleEndian.PutUint32(out[12:16], uint32(len(ref)))
	copy(out[16:], dir)
	copy(out[16+len(dir):], ref)
	return out
}

func TestModuleToolRecallAndStatsStages(t *testing.T) {
	h := NewHandler()
	dir := t.TempDir()
	ref := "tc-0123456789abcdef"
	want := []byte{'r', 'a', 'w', 0xff, '\n'}
	if err := os.WriteFile(filepath.Join(dir, ref+".out"), want, 0o600); err != nil {
		t.Fatal(err)
	}
	out, status := h(bus.ModuleInvocation{StageID: StageToolRecall}, recallRequest(dir, ref))
	if status != bus.ModuleStatusOK || len(out) < auxHeaderLen ||
		binary.LittleEndian.Uint16(out[6:8]) != recallOK ||
		string(out[auxHeaderLen:]) != string(want) {
		t.Fatalf("recall response=%x status=%v", out, status)
	}

	out, status = h(bus.ModuleInvocation{StageID: StageToolRecall}, recallRequest(dir, "../bad"))
	if status != bus.ModuleStatusOK || binary.LittleEndian.Uint16(out[6:8]) != recallInvalidRef {
		t.Fatalf("invalid ref response=%x status=%v", out, status)
	}

	out, status = h(bus.ModuleInvocation{StageID: StageToolStats}, nil)
	if status != bus.ModuleStatusOK || len(out) != statsResponseLen ||
		binary.LittleEndian.Uint32(out[0:4]) != statsMagic {
		t.Fatalf("stats response=%x status=%v", out, status)
	}
	if got := int64(binary.LittleEndian.Uint64(out[56:64])); got == 0 {
		t.Fatal("successful recall was not reflected in recovered counter")
	}
}

func TestModuleToolRecallRejectsMalformedFramesAndDirectories(t *testing.T) {
	h := NewHandler()
	ref := "tc-0123456789abcdef"
	for _, req := range [][]byte{
		[]byte("short"),
		recallRequest("relative", ref),
		recallRequest("/tmp/../etc", ref),
		recallRequest("/tmp/\x00hidden", ref),
	} {
		if _, status := h(bus.ModuleInvocation{StageID: StageToolRecall}, req); status != bus.ModuleStatusInvalidRequest {
			t.Errorf("request %q: status=%v", req, status)
		}
	}
	badVersion := recallRequest(t.TempDir(), ref)
	binary.LittleEndian.PutUint16(badVersion[4:6], auxWireVersion+1)
	if _, status := h(bus.ModuleInvocation{StageID: StageToolRecall}, badVersion); status != bus.ModuleStatusInvalidRequest {
		t.Errorf("bad version: status=%v", status)
	}
}

func TestModuleRecordBuildStage(t *testing.T) {
	h := NewHandler()
	messages, _ := json.Marshal([]map[string]string{
		{"role": "user", "content": "please inspect /untrusted/input.txt " + strings.Repeat("context ", 100)},
		{"role": "assistant", "content": "[done] changed src/server/session_compact.c at port=3002"},
		{"role": "assistant", "content": "[blocked] denied by policy"},
	})
	req := RecordBuildRequest{
		Messages: messages,
		Start:    0,
		End:      3,
	}
	body, _ := json.Marshal(req)
	out, status := h(bus.ModuleInvocation{StageID: StageRecordBuild}, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status=%v body=%s", status, out)
	}
	var got recordBuildResponse
	if err := json.Unmarshal(out, &got); err != nil {
		t.Fatal(err)
	}
	if len(got.Record.Decisions) != 1 || len(got.Record.Errors) != 1 ||
		len(got.Record.Files) != 2 || !strings.Contains(got.Closet, "(untrusted)") ||
		!strings.Contains(got.Closet, "3002 ⟦port⟧") {
		t.Fatalf("record response=%+v", got)
	}

	req.End = 4
	body, _ = json.Marshal(req)
	if _, status = h(bus.ModuleInvocation{StageID: StageRecordBuild}, body); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("bad range status=%v", status)
	}
}

// The event kind is fixed by the process contract at 4096 + ordinal*256 + stage.
func TestModuleEventKindMatchesContract(t *testing.T) {
	const ordinal = 27
	events := []uint32{EventReduce, EventJSONCompact, EventToolRecall, EventToolStats, EventRecordBuild}
	for index, event := range events {
		stage := uint32(index + 1)
		if want := uint32(4096 + ordinal*256 + stage); event != want {
			t.Errorf("event %d = %d, want %d", stage, event, want)
		}
	}
}

var errStoreDown = errors.New("store is unreachable")
