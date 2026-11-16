package ir

import (
	"fmt"
	"strings"
	"testing"
)

func refusalEvent(payload string) []byte {
	return []byte(`{"kind":"delta","message_id":"m","role":"assistant","payload":` + payload + `}`)
}

func TestRefusalSessionAdmission(t *testing.T) {
	for _, p := range []string{
		`{"type":"TURN_STOP","stop_reason":"content_filter","raw_stop_reason":"refusal","refusal":{"category":"future_category","explanation":"Diagnostic only; not a permission or retry instruction"}}`,
		`{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"explanation":null}}`,
		`{"type":"TURN_STOP","stop_reason":"end_turn","refusal":null}`,
		`{"type":"TURN_STOP","stop_reason":"end_turn"}`,
	} {
		if !ValidSessionEvent(refusalEvent(p)) {
			t.Errorf("valid refusal metadata rejected: %s", p)
		}
	}
	for name, p := range map[string]string{
		"wrong lifecycle":    `{"type":"TURN_START","refusal":null}`,
		"wrong stop":         `{"type":"TURN_STOP","stop_reason":"end_turn","refusal":{"category":null,"explanation":null}}`,
		"missing field":      `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null}}`,
		"unknown field":      `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"explanation":null,"retry":true}}`,
		"duplicate":          `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"category":"x","explanation":null}}`,
		"unpaired surrogate": `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"explanation":"\ud800"}}`,
		"number":             `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":2,"explanation":null}}`,
		"NUL":                `{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"explanation":"bad\u0000value"}}`,
		"category bytes":     fmt.Sprintf(`{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":%q,"explanation":null}}`, strings.Repeat("é", 33)),
		"explanation bytes":  fmt.Sprintf(`{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":null,"explanation":%q}}`, strings.Repeat("é", 2049)),
	} {
		t.Run(name, func(t *testing.T) {
			if ValidSessionEvent(refusalEvent(p)) {
				t.Fatal("malformed or misplaced metadata admitted")
			}
		})
	}
	if !ValidSessionEvent(refusalEvent(fmt.Sprintf(`{"type":"TURN_STOP","stop_reason":"content_filter","refusal":{"category":%q,"explanation":%q}}`, strings.Repeat("é", 32), strings.Repeat("é", 2048)))) {
		t.Fatal("exact UTF8 byte bounds rejected")
	}
}
