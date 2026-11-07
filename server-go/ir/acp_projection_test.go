package ir

import "testing"

func TestExternalACPProjectionRequiresActualBoundedObservations(t *testing.T) {
	base := `{"kind":"projection","payload":{"model":"external-acp","boundary":"request","omitted_reasoning_blocks":0,"omitted_opaque_state_blocks":0,`
	for _, field := range []string{"omitted_empty_messages", "omitted_request_controls", "host_context_blocks_as_text", "unverified_external_tool_reports", "history_messages_as_text"} {
		other := "host_context_blocks_as_text"
		if field == other {
			other = "unverified_external_tool_reports"
		}
		for _, invalid := range []string{"null", "true", `"1"`, "0.5"} {
			if ValidSessionEvent([]byte(base + `"` + field + `":` + invalid + `,"` + other + `":1}}`)) {
				t.Fatalf("accepted malformed optional count beside a valid observation: %s=%s", field, invalid)
			}
		}
		for _, value := range []string{"1", "4096"} {
			if !ValidSessionEvent([]byte(base + `"` + field + `":` + value + `}}`)) {
				t.Fatalf("refused real count %s=%s", field, value)
			}
		}
		for _, value := range []string{"-1", "0", "0.5", "4097", "true", `"1"`, "null"} {
			if ValidSessionEvent([]byte(base + `"` + field + `":` + value + `}}`)) {
				t.Fatalf("accepted false count %s=%s", field, value)
			}
		}
	}
}
