package families

import (
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"strings"
	"testing"
)

func sourceHistoryFixture(t *testing.T) []canonical.Message {
	t.Helper()
	var event struct {
		Kind      string            `json:"kind"`
		MessageID string            `json:"message_id"`
		Payload   canonical.Message `json:"payload"`
	}
	if !canonical.Decode([]byte(sourceResultEvent(t, "call", "exact native output α 9007199254740993")), &event) {
		t.Fatal("fixture decode")
	}
	return []canonical.Message{{Role: "assistant", Blocks: []canonical.Block{{Type: "tool_use", ToolID: "call", Name: "memory_serve", Input: json.RawMessage(`{"sequence":9007199254740993}`)}}}, event.Payload}
}
func copySourceHistory(t *testing.T, h []canonical.Message) []canonical.Message {
	t.Helper()
	b, e := json.Marshal(h)
	if e != nil {
		t.Fatal(e)
	}
	v, e := canonical.DecodeMessages(b)
	if e != nil {
		t.Fatal(e)
	}
	return v
}
func TestToolSourceHistoryPreservationExactChronologyAndWideInputs(t *testing.T) {
	current := sourceHistoryFixture(t)
	classified := copySourceHistory(t, current)
	classified[0].Blocks[0].Context = &canonical.Context{Origin: 1, Authority: 2, Trust: 3, ModelVisible: true}
	classified[1].Blocks[0].Context = &canonical.Context{Origin: 4, Authority: 5, Trust: 6, ModelVisible: true}
	if !toolSourceHistoryPreserved(current, classified) {
		t.Fatal("host classification presentation metadata changed source identity")
	}
	if !toolSourceHistoryPreserved(current, copySourceHistory(t, current)) {
		t.Fatal("identical committed receipt refused")
	}
	if !toolSourceHistoryPreserved(current, []canonical.Message{}) {
		t.Fatal("whole cycle removal refused")
	}
	for mutation := 0; mutation < 8; mutation++ {
		next := copySourceHistory(t, current)
		switch mutation {
		case 0:
			next[1].Blocks[0].ToolSource = nil
		case 1:
			next[1].Blocks[0].ToolSource.Receipt = `{"changed":true}`
		case 2:
			next[0].Blocks[0].Name = "ordinary"
		case 3:
			next[0].Blocks[0].Namespace = "foreign"
		case 4:
			next[0].Blocks[0].Input = json.RawMessage(`{"sequence":9007199254740992}`)
		case 5:
			next = append(next, copySourceHistory(t, current)...)
		case 7:
			next[1].Blocks[0].ToolSource = nil
			next = next[1:]
		case 6:
			next[1].Blocks[0].Content = json.RawMessage(`"changed"`)
		}
		if toolSourceHistoryPreserved(current, next) {
			t.Fatalf("mutation%d admitted", mutation)
		}
	}
	ordinary := copySourceHistory(t, current)
	ordinary[0].Blocks[0].Name = "business"
	ordinary[1].Blocks[0].ToolSource = nil
	original := append(copySourceHistory(t, current), ordinary...)
	if !toolSourceHistoryPreserved(original, ordinary) {
		t.Fatal("distinct committed reused-ID ordinary cycle refused")
	}
	if toolSourceHistoryPreserved([]canonical.Message{}, current) {
		t.Fatal("new source receipt admitted")
	}
	if !Sessions.Ops[opSessionToolSourceHistorySave].HostOnly {
		t.Fatal("checkpoint not actualCore0 only")
	}
}

func TestToolSourceHistoryIgnoresUnrelatedBusinessStateButRejectsRenamedCopiedBodies(t *testing.T) {
	source := sourceHistoryFixture(t)
	business := canonical.Message{Role: "assistant", Blocks: []canonical.Block{{Type: "tool_use", ToolID: "business", Name: "ordinary_business", Input: json.RawMessage(`{}`)}}}
	repeated := []canonical.Message{business, business}
	if !toolSourceHistoryPreserved(repeated, repeated) {
		t.Fatal("sourcefree ordinary history gained a new unfinished-call constraint")
	}
	mixed := append(copySourceHistory(t, source), repeated...)
	if !toolSourceHistoryPreserved(mixed, mixed) {
		t.Fatal("unrelated sourcefree business IDs affected source custody")
	}
	if !toolSourceHistoryPreserved(mixed, repeated) {
		t.Fatal("deliberate whole source cycle removal refused")
	}
	for _, omitCall := range []bool{false, true} {
		copied := copySourceHistory(t, source)
		copied[0].Blocks[0].ToolID = "renamed-business"
		copied[0].Blocks[0].Name = "ordinary_business"
		copied[1].Blocks[0].ToolID = "renamed-business"
		copied[1].Blocks[0].ToolSource = nil
		if omitCall {
			copied = copied[1:]
		}
		if toolSourceHistoryPreserved(mixed, copied) {
			t.Fatal("exact protected body lost custody after identity/receipt stripping")
		}
	}
	businessResult := canonical.Message{Role: "tool", Blocks: []canonical.Block{{Type: "tool_result", ToolID: "unrelated", Content: json.RawMessage(`"unrelated ordinary output"`)}}}
	if !toolSourceHistoryPreserved(mixed, append(copySourceHistory(t, source), businessResult)) {
		t.Fatal("unrelated sourcefree result gained authority constraints")
	}
}

func TestToolSourceHistoryProtectedBodyUsesVisibleProjectionWithoutRoundingWideJSON(t *testing.T) {
	for _, text := range []string{`{"wide":9007199254740993,"text":"source"}`, `[9007199254740993,"source"]`} {
		current := sourceHistoryFixture(t)
		var event struct {
			Kind      string            `json:"kind"`
			MessageID string            `json:"message_id"`
			Payload   canonical.Message `json:"payload"`
		}
		if !canonical.Decode([]byte(sourceResultEvent(t, "call", text)), &event) {
			t.Fatal("fixture source decode")
		}
		current[1] = event.Payload
		for _, content := range []json.RawMessage{json.RawMessage(text), json.RawMessage("  " + text + " ")} {
			copied := copySourceHistory(t, current)
			copied[0].Blocks[0].ToolID = "renamed-business"
			copied[0].Blocks[0].Name = "ordinary"
			copied[1].Blocks[0].ToolID = "renamed-business"
			copied[1].Blocks[0].ToolSource = nil
			copied[1].Blocks[0].Content = content
			if toolSourceHistoryPreserved(current, copied) {
				t.Fatal("same visible protected body escaped through structured JSON type")
			}
		}
		stringBytes, _ := json.Marshal(text)
		a, ok := sourceHistoryVisibleDigest(stringBytes)
		if !ok {
			t.Fatal("string projection")
		}
		b, ok := sourceHistoryVisibleDigest(json.RawMessage(text))
		if !ok || a != b {
			t.Fatal("same visible projection differs")
		}
		changed := strings.Replace(text, "9007199254740993", "9007199254740992", 1)
		c, ok := sourceHistoryVisibleDigest(json.RawMessage(changed))
		if !ok || a == c {
			t.Fatal("wide JSON body rounded or conflated")
		}
	}
}
