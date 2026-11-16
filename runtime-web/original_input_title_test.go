package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// Full actual handleChatSend/SQLite route fixture. The downstream C producer is
// intentionally a fake UDS HTTP peer: this test proves metadata ordering only.
func TestOriginalInputModeDoesNotPersistRawDerivedTitle(t *testing.T) {
	for _, enabled := range []bool{false, true} {
		for _, downstream := range []int{200, 503} {
			t.Run(fmt.Sprintf("enabled_%v_downstream_%d", enabled, downstream), func(t *testing.T) {
				s := newSessionTestServer(t)
				s.originalInputMode = enabled
				workspace := t.TempDir()
				mux := http.NewServeMux()
				mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
					if enabled {
						w.Header().Set("X-Aimee-Original-Input", "required")
					} else {
						w.Header().Set("X-Aimee-Original-Input", "optional")
					}
					fmt.Fprintf(w, `{"root":%q}`, workspace)
				})
				canary := "synthetic.selected.value@fixture.test"
				calls := 0
				mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
					calls++
					if r.Header.Get("X-Aimee-Webuser") != "alice" {
						t.Fatal("unverified header reached owner")
					}
					var body map[string]any
					if e := json.NewDecoder(r.Body).Decode(&body); e != nil {
						t.Fatal(e)
					}
					if body["message"].(map[string]any)["blocks"].([]any)[0].(map[string]any)["text"] != canary {
						t.Fatal("title fix must not silently alter input bytes")
					}
					cs, e := s.getChatSession("alice", oiSID)
					if e != nil || cs == nil {
						t.Fatal(e)
					}
					if enabled && cs.Title != "" {
						t.Fatal("raw-derived title already persisted before downstream", cs.Title)
					}
					w.WriteHeader(downstream)
					if downstream == 200 {
						fmt.Fprintln(w, `{"version":1}`)
					}
				})
				s.cfg = startFakeV1(t, mux)
				r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(fmt.Sprintf(`{"aimee_session_id":%q,"message":{"role":"user","blocks":[{"type":"text","text":%q}]},"original_input_operation_uuid":%q}`, oiSID, canary, oiUUID)))
				r.Header.Set("X-Aimee-Webuser", "mallory")
				addWebSession(t, s, r, "alice")
				w := httptest.NewRecorder()
				s.requireAuth(s.handleChatSend)(w, r)
				if calls != 1 {
					t.Fatal(calls, w.Code, w.Body.String())
				}
				cs, e := s.getChatSession("alice", oiSID)
				if e != nil || cs == nil {
					t.Fatal(e)
				}
				want := canary
				if enabled {
					want = ""
				}
				if cs.Title != want {
					t.Fatal(cs.Title, want)
				}
				if len(cs.Messages) != 0 {
					t.Fatal("raw transcript copied")
				}
			})
		}
	}
}
