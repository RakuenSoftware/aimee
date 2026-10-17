package families

import "testing"

func TestSessionIRCanonicalValidation(t *testing.T) {
	for _, event := range []string{
		`{"kind":"message","message_id":"u","payload":{"role":"user","blocks":[{"type":"text","text":"hello"},{"type":"image","media_type":"image/png","media_ref":"artifact:a"}]}}`,
		`{"kind":"message","message_id":"a","payload":{"role":"assistant","blocks":[{"type":"thinking","text":"reason","signature":"opaque"},{"type":"tool_use","tool_id":"a","name":"bash","input":{"command":"pwd"}}]}}`,
		`{"kind":"message","message_id":"t","payload":{"role":"tool","blocks":[{"type":"tool_result","tool_id":"a","content":{"stdout":"/workspace"},"is_error":false}]}}`,
		`{"kind":"delta","message_id":"a","role":"assistant","payload":{"type":"BLOCK_DELTA","kind":"tool_use","block_id":0,"tool_args_delta":"{"}}`,
		`{"kind":"delta","message_id":"a","role":"assistant","payload":{"type":"TURN_STOP","usage_in":12,"usage_out":5,"stop_reason":"tool_use"}}`,
		`{"kind":"control_request","request_id":"p","control_kind":"permission","payload":{"options":["allow","deny"]}}`,
		`{"kind":"control_response","request_id":"p","control_kind":"permission","payload":{"decision":"deny"}}`,
		`{"kind":"execution","payload":{"status":"running"}}`,
		`{"kind":"execution","payload":{"status":"failed","error":"runtime unavailable"}}`,
	} {
		if !validSessionIREvent([]byte(event)) {
			t.Errorf("rejected canonical event %s", event)
		}
	}
	for _, event := range []string{
		`null`, `[]`, `{}`, `{"kind":"message","payload":null}`,
		`{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":null}}`,
		`{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"native","text":"lost tool"}]}}`,
		`{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"tool_use","name":"bash"}]}}`,
		`{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"text","text":42}]}}`,
		`{"kind":"message","message_id":"m","payload":{"role":"assistant","blocks":[{"type":"text","text":"x","raw":{"native":"sidecar"}}]}}`,
		`{"kind":"delta","message_id":"m","role":"assistant","payload":{"type":"BLOCK_START","block_id":0,"kind":"tool_use"}}`,
		`{"kind":"delta","message_id":"m","role":"assistant","payload":{"type":"BLOCK_DELTA","block_id":0.5,"kind":"text","text_delta":"x"}}`,
		`{"kind":"delta","message_id":"m","role":"assistant","payload":{"type":"BLOCK_DELTA","block_id":0,"kind":"text","tool_args_delta":"{"}}`,
		`{"kind":"delta","message_id":"m","role":"assistant","payload":{"type":"TURN_STOP","usage_in":-1}}`,
		`{"kind":"control_response","control_kind":"permission","payload":{}}`,
		`{"kind":"control_request","request_id":"p","control_kind":"permission","payload":[]}`,
		`{"kind":"execution","payload":{"status":"invented"}}`,
		`{"kind":"execution","payload":{"status":"running"}} {}`,
	} {
		if validSessionIREvent([]byte(event)) {
			t.Errorf("accepted malformed event %s", event)
		}
	}
}
