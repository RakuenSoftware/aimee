package main

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

const oiUUID = "12345678-1234-4234-8234-123456789abc"
const oiSID = "web-abcdef12-1234-4234-8234-123456789abc"

func TestOIShapes(t *testing.T) {
	operation := json.RawMessage(`"` + oiUUID + `"`)
	for _, sid := range []string{oiSID, "web-mabcdef1-a1b2c3d4"} {
		if _, e := originalInputTransport("a☃b", sid, operation, json.RawMessage(`[{"offset":1,"length":3}]`)); e != nil {
			t.Fatal(sid, e)
		}
	}
	for _, raw := range []string{`null`, `{}`, `[{"offset":0,"length":0}]`, `[{"offset":-1,"length":1}]`, `[{"offset":0,"length":8193}]`, `[{"offset":1,"length":1}]`, `[{"offset":2,"length":2}]`, `[{"offset":0,"length":1},{"offset":0,"length":1}]`, `[{"offset":0,"length":1,"extra":true}]`, `[{"offset":0,"offset":1,"length":1}]`, `[{"offset":0.5,"length":1}]`, `[{"offset":0,"length":1e0}]`, `[{"offset":9223372036854775808,"length":1}]`, `[{"offset":0}]`, `[null]`, `[[]]`} {
		if _, e := originalInputTransport("a☃b", oiSID, operation, json.RawMessage(raw)); e == nil {
			t.Fatal("accepted", raw)
		}
	}
	spans := `[` + strings.TrimSuffix(strings.Repeat(`{"offset":0,"length":1},`, 33), ",") + `]`
	if _, e := originalInputTransport(strings.Repeat("a", 33), oiSID, operation, json.RawMessage(spans)); e == nil {
		t.Fatal("accepted too many spans")
	}
	for _, id := range []string{"", `null`, `7`, `"00000000-0000-0000-0000-000000000000"`, `"12345678-1234-4234-8234-123456789ABC"`, `"text"`} {
		if _, e := originalInputTransport("hello", oiSID, json.RawMessage(id), nil); e == nil {
			t.Fatal("accepted operation", id)
		}
	}
	for _, sid := range []string{"owned-session", "web-", "web-one", "web-name/path-x", strings.Repeat("x", 128), " " + oiSID} {
		if _, e := originalInputTransport("hello", sid, operation, nil); e == nil {
			t.Fatal("accepted strict SID", sid)
		}
	}
	if _, e := originalInputTransport(strings.Repeat("a", 262145), oiSID, operation, nil); e == nil {
		t.Fatal("accepted oversized text")
	}
}
func TestOIForward(t *testing.T) {
	for _, enabled := range []bool{false, true} {
		t.Run(fmt.Sprint(enabled), func(t *testing.T) {
			s := newSessionTestServer(t)
			s.originalInputMode = enabled
			root := t.TempDir()
			calls := 0
			mux := http.NewServeMux()
			mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
				if enabled {
					w.Header().Set("X-Aimee-Original-Input", "required")
				} else {
					w.Header().Set("X-Aimee-Original-Input", "optional")
				}
				fmt.Fprintf(w, `{"root":%q}`, root)
			})
			mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
				calls++
				var body map[string]json.RawMessage
				if e := json.NewDecoder(r.Body).Decode(&body); e != nil {
					t.Fatal(e)
				}
				if string(body["message"]) != `{"role":"user","blocks":[{"type":"text","text":"a☃b"}]}` {
					t.Fatal(string(body["message"]))
				}
				_, hasID := body["original_input_operation_uuid"]
				_, hasSpans := body["protected_spans"]
				if hasID != enabled || hasSpans != enabled {
					t.Fatal(body)
				}
				if enabled && string(body["original_input_operation_uuid"]) != `"`+oiUUID+`"` {
					t.Fatal(body)
				}
				if r.Header.Get("X-Aimee-Webuser") != "alice" {
					t.Fatal("wrong request user")
				}
				fmt.Fprintln(w, `{"version":1}`)
			})
			s.cfg = startFakeV1(t, mux)
			sid := oiSID
			if !enabled {
				sid = "legacy-session-label"
			}
			body := fmt.Sprintf(`{"aimee_session_id":%q,"message":{"role":"user","blocks":[{"type":"text","text":"a☃b"}]},"original_input_operation_uuid":%q,"protected_spans":[{"offset":1,"length":3}]}`, sid, oiUUID)
			r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(body))
			addWebSession(t, s, r, "alice")
			w := httptest.NewRecorder()
			s.requireAuth(s.handleChatSend)(w, r)
			if calls != 1 || w.Code != 200 {
				t.Fatal(calls, w.Code, w.Body.String())
			}
		})
	}
}
func TestOIAckMetadata(t *testing.T) {
	for _, ack := range []string{"none", "wrong-session", "assistant", "no-attachment", "wrong-operation", "bad-hash", "multiple-blocks", "user"} {
		t.Run(ack, func(t *testing.T) {
			s := newSessionTestServer(t)
			s.originalInputMode = true
			root := t.TempDir()
			raw := "Browser draft wording"
			safe := "Committed server wording"
			mux := http.NewServeMux()
			mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
				w.Header().Set("X-Aimee-Original-Input", "required")
				fmt.Fprintf(w, `{"root":%q}`, root)
			})
			mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
				cs, e := s.getChatSession("alice", oiSID)
				if e != nil || cs == nil || cs.Cwd != "" || cs.Title != "" {
					t.Fatal("metadata persisted before ACK", cs, e)
				}
				if ack == "none" {
					w.WriteHeader(503)
					return
				}
				sid, role := oiSID, "user"
				if ack == "wrong-session" {
					sid = "web-11111111-1111-4111-8111-111111111111"
				}
				if ack == "assistant" {
					role = "assistant"
				}
				hash := sha256.Sum256([]byte(safe))
				operation, digest := oiUUID, hex.EncodeToString(hash[:])
				if ack == "wrong-operation" {
					operation = "22345678-1234-4234-8234-123456789abc"
				}
				if ack == "bad-hash" {
					digest = strings.Repeat("0", 64)
				}
				block := map[string]any{"type": "text", "text": safe}
				if ack != "no-attachment" && ack != "assistant" {
					block["input_attachment"] = map[string]any{"schema_version": 1, "kind": "host.original-input.v1", "operation_uuid": operation, "text_sha256": digest, "opaque": "opaque-documentary-fixture"}
				}
				blocks := []any{block}
				if ack == "multiple-blocks" {
					blocks = append(blocks, map[string]any{"type": "text", "text": "extra"})
				}
				json.NewEncoder(w).Encode(map[string]any{"version": 1, "session_id": sid, "sequence": "2", "event_id": "user-2", "turn_id": "turn", "actor": "transport-owner", "event": map[string]any{"kind": "message", "message_id": "u", "payload": map[string]any{"role": role, "blocks": blocks}}})
			})
			s.cfg = startFakeV1(t, mux)
			r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(fmt.Sprintf(`{"aimee_session_id":%q,"message":%q,"cwd":%q,"original_input_operation_uuid":%q}`, oiSID, raw, root, oiUUID)))
			addWebSession(t, s, r, "alice")
			w := httptest.NewRecorder()
			s.requireAuth(s.handleChatSend)(w, r)
			cs, e := s.getChatSession("alice", oiSID)
			if e != nil || cs == nil {
				t.Fatal(e)
			}
			title, cwd := "", ""
			if ack == "user" {
				title = safe
				cwd = root
			}
			if cs.Title != title || cs.Cwd != cwd {
				t.Fatal(cs.Title, cs.Cwd, title, cwd)
			}
			if len(cs.Messages) != 0 {
				t.Fatal("transport copied transcript")
			}
		})
	}
}
func TestOIRejectedMetadata(t *testing.T) {
	s := newSessionTestServer(t)
	s.originalInputMode = true
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Aimee-Original-Input", "required")
		fmt.Fprint(w, `{"root":"/work"}`)
	})
	s.cfg = startFakeV1(t, mux)
	for _, body := range []string{`{"aimee_session_id":"user-chosen-title","message":"hello","original_input_operation_uuid":"` + oiUUID + `"}`, `{"aimee_session_id":"` + oiSID + `","message":"hello"}`, `{"aimee_session_id":"` + oiSID + `","message":"hello","original_input_operation_uuid":"` + oiUUID + `","protected_spans":[{"offset":0,"length":99}]}`} {
		r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(body))
		addWebSession(t, s, r, "alice")
		w := httptest.NewRecorder()
		s.requireAuth(s.handleChatSend)(w, r)
		if w.Code != 400 {
			t.Fatal(w.Code, w.Body.String())
		}
	}
	var count int
	if e := s.db.QueryRow(`SELECT COUNT(*) FROM chat_sessions`).Scan(&count); e != nil || count != 0 {
		t.Fatal(count, e)
	}
}
