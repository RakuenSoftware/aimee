package economizer

import (
	"encoding/json"
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

// State round-trips through the stage, so a conversation keeps its freeze
// boundary and page table across turns.
func TestModuleStateRoundTrip(t *testing.T) {
	first, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
		RecallEnabled: true,
		Turn:          1,
	})
	if status != bus.ModuleStatusOK || first.State == "" {
		t.Fatalf("expected serialized state back: %+v", first)
	}

	second, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
		RecallEnabled: true,
		State:         first.State,
		Turn:          2,
	})
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	if second.State == "" {
		t.Error("state should survive a second turn")
	}
}

// An unreadable state is DISCARDED, not fatal: the reduction still runs, it just
// starts cold. Failing the request would cost more than one turn of warmth.
func TestModuleBadStateIsNotFatal(t *testing.T) {
	resp, status := invoke(t, ReduceRequest{
		Messages:      rawMessages(t, 20),
		Seam:          "delegate",
		HistoryFold:   true,
		ClosetEnabled: true,
		State:         "{not valid",
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

// The event kind is fixed by the process contract at 4096 + ordinal*256 + stage.
func TestModuleEventKindMatchesContract(t *testing.T) {
	const ordinal = 27
	if want := uint32(4096 + ordinal*256 + 1); EventReduce != want {
		t.Errorf("EventReduce = %d, want %d", EventReduce, want)
	}
	if StageReduce != 1 {
		t.Errorf("StageReduce = %d, want 1", StageReduce)
	}
}
