package families

import (
	"encoding/json"
	canonical "github.com/JBailes/aimee/server-go/ir"
	"testing"
)

func TestCoordQueueMessageCustodyAndLegacyStorageImport(t *testing.T) {
	original := ` {"role":"user","blocks":[{"type":"text","text":"look here"},{"type":"image","media_type":"image/png","media_ref":"https://fixture.invalid/a.png"}]} `
	for _, legacy := range []bool{false, true} {
		got, ok := coordTaskMessage(original, legacy)
		if !ok || got != original {
			t.Fatalf("canonical bytes were changed or refused: %q", got)
		}
	}
	text := "Legacy task: 雪\nquotes: \"task\""
	if _, ok := coordTaskMessage(text, false); ok {
		t.Fatal("new queue write accepted plain text")
	}
	got, ok := coordTaskMessage(text, true)
	var message canonical.Message
	if !ok || !canonical.ValidMessage(json.RawMessage(got)) || json.Unmarshal([]byte(got), &message) != nil || len(message.Blocks) != 1 || message.Blocks[0].Text == nil || *message.Blocks[0].Text != text {
		t.Fatalf("legacy storage import changed text: %q", got)
	}
	for _, bad := range []string{`{"role":"user","blocks":"invalid"}`, `{"role":"assistant","blocks":[{"type":"text","text":"wrong role"}]}`, ""} {
		for _, legacy := range []bool{false, true} {
			if _, ok := coordTaskMessage(bad, legacy); ok {
				t.Fatalf("invalid task accepted: %q", bad)
			}
		}
	}
}
