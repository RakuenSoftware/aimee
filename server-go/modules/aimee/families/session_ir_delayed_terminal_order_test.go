package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	canonical "github.com/JBailes/aimee/server-go/ir"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"reflect"
	"strings"
	"testing"
)

// Controlled stored rows exercise the real reducer, not browser/Native authority.
func delayedAttachment(t *testing.T, n int) (string, canonical.Message) {
	t.Helper()
	text := fmt.Sprintf("owned safe input %d", n)
	sum := sha256.Sum256([]byte(text))
	a := &canonical.InputAttachment{SchemaVersion: 1, Kind: "host.original-input.v1", OperationUUID: fmt.Sprintf("12345678-1234-4234-8234-%012d", n), TextSHA256: hex.EncodeToString(sum[:]), Opaque: "  {\"wide\":9007199254740993}\n"}
	m := canonical.Message{Role: "user", Blocks: []canonical.Block{{Type: "text", Text: &text, InputAttachment: a}}}
	raw, err := json.Marshal(map[string]any{"kind": "message", "message_id": "user", "payload": m})
	if err != nil {
		t.Fatal(err)
	}
	return string(raw), m
}
func delayedHistory(t *testing.T, events [][2]string, active string) ([]canonical.Message, string, error) {
	t.Helper()
	db := newSessDB()
	db.rows = runningHistoryRows(events...)
	status, cells, err := sessionIRHistoryLoad(context.Background(), db, []string{"s", "primary", "owner", active})
	if err != nil {
		return nil, "", err
	}
	if status != store.StatusOK || len(cells) != 1 {
		t.Fatalf("history status=%d cells=%d", status, len(cells))
	}
	var result []canonical.Message
	if err = json.Unmarshal([]byte(cells[0]), &result); err != nil {
		t.Fatal(err)
	}
	return result, cells[0], nil
}
func TestDelayedTerminalRetainsOriginalInputOrderAndCurrentTurnGuard(t *testing.T) {
	a, am := delayedAttachment(t, 1)
	b, bm := delayedAttachment(t, 2)
	current, _ := delayedAttachment(t, 3)
	events := [][2]string{{"early", runningStart}, {"early", a}, {"later", runningStart}, {"later", b}, {"later", `{"kind":"execution","payload":{"status":"failed"}}`}, {"early", `{"kind":"execution","payload":{"status":"cancelled"}}`}, {"current", runningStart}, {"current", current}}
	got, _, err := delayedHistory(t, events, "current")
	if err != nil {
		t.Fatal(err)
	}
	if !reflect.DeepEqual(got, []canonical.Message{am, bm}) {
		t.Fatal("late terminal reordered/changed/duplicated admitted input")
	}
	if _, _, err = delayedHistory(t, events, "foreign"); !errors.Is(err, errSessionIRUnfinishedExecution) {
		t.Fatal("foreign active turn lost unfinished guard", err)
	}
	pending := append(append([][2]string{}, events[:5]...), events[6:]...)
	if _, _, err = delayedHistory(t, pending, "current"); !errors.Is(err, errSessionIRUnfinishedExecution) {
		t.Fatal("unfinished earlier source became visible", err)
	}
}
func TestDelayedTerminalRemapsNativeInterruptionToExactCall(t *testing.T) {
	a, am := delayedAttachment(t, 1)
	b, bm := delayedAttachment(t, 2)
	owner := canonical.ToolInterruptionOwner{Owner: strings.Repeat("a", 32), Binding: strings.Repeat("b", 64), Epoch: "0", Containment: strings.Repeat("c", 64)}
	nativeOwner, _ := json.Marshal(canonical.NativeExecutionOwner{Version: 1, Harness: "codex", ToolInterruptionOwner: owner})
	call := `{"kind":"message","message_id":"a","payload":{"role":"assistant","blocks":[{"type":"tool_use","tool_id":"exact-call","name":"bash","input":{"wide":123.123456789012345678901234567890}}]},"native_owner":` + string(nativeOwner) + `}`
	receipt := canonical.ToolInterruption{Version: 1, Source: canonical.ToolInterruptionSource{Session: "s", Turn: "early", Sequence: "3", EventID: "event-3", Actor: "owner", MessageID: "a", BlockIndex: 0, ToolID: "exact-call", Name: "bash"}, Owner: owner, Terminal: "7", Outcome: "unknown", Retry: "explicit_permission_required"}
	raw, _ := json.Marshal(map[string]any{"kind": "tool_interruption", "payload": receipt})
	events := [][2]string{{"early", runningStart}, {"early", a}, {"early", call}, {"later", runningStart}, {"later", b}, {"later", `{"kind":"execution","payload":{"status":"failed"}}`}, {"early", runningDone}, {"early", string(raw)}}
	got, wire, err := delayedHistory(t, events, "")
	if err != nil {
		t.Fatal(err)
	}
	if len(got) != 4 || !reflect.DeepEqual(got[0], am) || got[1].Role != "assistant" || got[1].Blocks[0].ToolID != "exact-call" || got[2].Role != "tool" || got[2].Blocks[0].ToolID != "exact-call" || got[2].Blocks[0].Context.RevisionDomain != canonical.ToolInterruptionDomain || !reflect.DeepEqual(got[3], bm) {
		t.Fatal("native interruption remapped onto another turn/attachment")
	}
	if !strings.Contains(wire, "123.123456789012345678901234567890") {
		t.Fatal("exact original call bytes changed")
	}
	forged := receipt
	forged.Source.Sequence = "5"
	bad, _ := json.Marshal(map[string]any{"kind": "tool_interruption", "payload": forged})
	events[7][1] = string(bad)
	if _, _, err = delayedHistory(t, events, ""); err == nil {
		t.Fatal("foreign source index accepted after permutation")
	}
}
func TestDelayedTerminalPreservesCheckpointAndImportedPrefix(t *testing.T) {
	for _, imported := range []bool{false, true} {
		t.Run(fmt.Sprint(imported), func(t *testing.T) {
			a, am := delayedAttachment(t, 1)
			b, bm := delayedAttachment(t, 2)
			prefix := `{"role":"assistant","blocks":[{"type":"text","text":"fixed historical prefix"}]}`
			events := [][2]string{}
			if imported {
				report := canonical.HistoryImportReport{Version: 1, SourceFormat: "canonical", Outcome: "exact", SourceSHA256: strings.Repeat("a", 64), SourceSession: "source", SourceAgent: "agent", SourceMessages: 1, ImportedMessages: 1, ToolStructure: "preserved"}
				raw, _ := json.Marshal(map[string]any{"kind": "history", "payload": map[string]any{"scope": "primary", "phase": "import_report", "import_report": report}})
				events = append(events, [2]string{"checkpoint", string(raw)})
			}
			events = append(events, [2]string{"checkpoint", `{"kind":"history","payload":{"scope":"primary","phase":"message","index":0,"message":` + prefix + `}}`}, [2]string{"checkpoint", `{"kind":"history","payload":{"scope":"primary","phase":"commit","count":1}}`}, [2]string{"early", runningStart}, [2]string{"early", a}, [2]string{"later", runningStart}, [2]string{"later", b}, [2]string{"later", `{"kind":"execution","payload":{"status":"failed"}}`}, [2]string{"early", runningDone})
			got, _, err := delayedHistory(t, events, "")
			if err != nil {
				t.Fatal(err)
			}
			if len(got) != 3 || got[0].Role != "assistant" || got[0].Blocks[0].Text == nil || *got[0].Blocks[0].Text != "fixed historical prefix" || !reflect.DeepEqual(got[1], am) || !reflect.DeepEqual(got[2], bm) {
				t.Fatal("accepted immutable prefix/source attachment changed")
			}
		})
	}
}

func TestDelayedTerminalOrderingValidatesBeforeRemappingSources(t *testing.T) {
	history := []json.RawMessage{json.RawMessage(`"fixed-prefix"`), json.RawMessage(`"later"`), json.RawMessage(`"early-call"`), json.RawMessage(`"early-result"`)}
	original := append([]json.RawMessage(nil), history...)
	refs := map[string]nativeHistorySource{"prefix": {Index: 0}, "later": {Index: 1}, "call": {Index: 2}, "unselected": {Index: -1}, "other-negative": {Index: -7}}
	ordered, err := orderSessionIRObservationTurns(history, []uint64{0, 9, 2, 2}, 1, refs)
	if err != nil {
		t.Fatal(err)
	}
	want := []json.RawMessage{history[0], history[2], history[3], history[1]}
	if !reflect.DeepEqual(ordered, want) || !reflect.DeepEqual(history, original) || refs["prefix"].Index != 0 || refs["later"].Index != 3 || refs["call"].Index != 1 || refs["unselected"].Index != -1 || refs["other-negative"].Index != -7 {
		t.Fatal("prefix/tool grouping/source indices not preserved")
	}
	for _, bad := range []struct {
		groups        []uint64
		prefix, index int
	}{{[]uint64{0, 9, 2}, 1, 1}, {[]uint64{0, 9, 2, 2}, -1, 1}, {[]uint64{0, 9, 2, 2}, 5, 1}, {[]uint64{0, 0, 2, 2}, 1, 1}, {[]uint64{0, 9, 2, 2}, 1, 4}} {
		input := map[string]nativeHistorySource{"valid": {Index: 2}, "probe": {Index: bad.index}, "negative": {Index: -1}}
		before := map[string]nativeHistorySource{}
		for k, v := range input {
			before[k] = v
		}
		if _, err := orderSessionIRObservationTurns(history, bad.groups, bad.prefix, input); err == nil {
			t.Fatal("invalid ordering shape/source admitted")
		}
		if !reflect.DeepEqual(input, before) || !reflect.DeepEqual(history, original) {
			t.Fatal("refusal partially mutated source indices/history")
		}
	}
}
