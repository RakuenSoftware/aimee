package families

import (
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	store "github.com/JBailes/aimee/server-go/modules/aimee"
	"testing"
)

func sourceResultEvent(t *testing.T, call, text string) string {
	t.Helper()
	hash := sha256.Sum256([]byte(text))
	source := map[string]any{"schema_version": 1, "owner": "70000000-0000-4000-8000-000000000007", "visible_sha256": hex.EncodeToString(hash[:]), "receipt": "{\"opaque\":true}"}
	event := map[string]any{"kind": "message", "message_id": "result", "payload": map[string]any{"role": "tool", "blocks": []any{map[string]any{"type": "tool_result", "tool_id": call, "content": text, "is_error": false, "tool_source": source}}}}
	raw, err := json.Marshal(event)
	if err != nil {
		t.Fatal(err)
	}
	return string(raw)
}
func TestToolSourceOrdinaryAppendAndCheckpointCannotAcquireCustody(t *testing.T) {
	if !Sessions.Ops[opSessionToolSourceAppend].HostOnly {
		t.Fatal("source append is not Core0-only")
	}
	db := newSessDB()
	payload := sourceResultEvent(t, "call", "exact native output")
	status, _, err := sessionIRAppend(context.Background(), db, []string{"s", "e", "turn", "owner", payload})
	if err != nil || status != store.StatusInvalid || len(db.executed) != 0 {
		t.Fatal(status, err, db.executed)
	}
	var event struct {
		Payload json.RawMessage `json:"payload"`
	}
	if err = json.Unmarshal([]byte(payload), &event); err != nil {
		t.Fatal(err)
	}
	messages, _ := json.Marshal([]json.RawMessage{event.Payload})
	status, _, err = sessionIRHistorySave(context.Background(), db, []string{"s", "checkpoint", "primary", "owner", string(messages)})
	if err != nil || status != store.StatusInvalid || len(db.executed) != 0 {
		t.Fatal(status, err, db.executed)
	}
}
