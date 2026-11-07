package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"sync/atomic"
	"testing"
)

func TestOriginalInputNegotiationBeforePersistence(t *testing.T) {
	cases := []struct {
		name      string
		explicit  bool
		headers   []string
		op        bool
		status    int
		forwarded bool
	}{
		{"required-forces-valid-metadata", false, []string{"required"}, true, 200, true},
		{"required-refuses-missing-action", false, []string{"required"}, false, 400, false},
		{"explicit-refuses-optional", true, []string{"optional"}, true, 503, false},
		{"explicit-refuses-unknown", true, nil, true, 503, false},
		{"invalid-header", false, []string{"invalid"}, true, 503, false},
		{"duplicate-header", false, []string{"required", "optional"}, true, 503, false},
		{"folded-header", false, []string{"required, optional"}, true, 503, false},
		{"legacy-optional", false, []string{"optional"}, false, 200, false},
		{"legacy-initial-unknown", false, nil, false, 200, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			s := newSessionTestServer(t)
			s.originalInputMode = tc.explicit
			root := t.TempDir()
			var calls atomic.Int32
			mux := http.NewServeMux()
			mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
				for _, v := range tc.headers {
					w.Header().Add("X-Aimee-Original-Input", v)
				}
				fmt.Fprintf(w, `{"root":%q}`, root)
			})
			mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				var body map[string]json.RawMessage
				if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
					t.Error(err)
					return
				}
				_, ok := body["original_input_operation_uuid"]
				if ok != tc.forwarded {
					t.Errorf("forwarded action=%v", ok)
				}
				if tc.forwarded {
					cs, e := s.getChatSession("alice", oiSID)
					if e != nil || cs == nil || cs.Title != "" || cs.Cwd != "" {
						t.Error("raw metadata persisted before acknowledgment", e)
					}
				}
			})
			s.cfg = startFakeV1(t, mux)
			body := fmt.Sprintf(`{"aimee_session_id":%q,"message":{"role":"user","blocks":[{"type":"text","text":"Synthetic browser wording"}]}}`, oiSID)
			if tc.op {
				body = fmt.Sprintf(`{"aimee_session_id":%q,"message":{"role":"user","blocks":[{"type":"text","text":"Synthetic browser wording"}]},"original_input_operation_uuid":%q}`, oiSID, oiUUID)
			}
			r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(body))
			addWebSession(t, s, r, "alice")
			w := httptest.NewRecorder()
			s.requireAuth(s.handleChatSend)(w, r)
			if w.Code != tc.status {
				t.Fatalf("status%d wanted%d: %s", w.Code, tc.status, w.Body.String())
			}
			want := int32(0)
			if tc.status == 200 {
				want = 1
			}
			if calls.Load() != want {
				t.Fatal("unexpected native dispatch", calls.Load())
			}
			var rows int
			if err := s.db.QueryRow(`SELECT COUNT(*) FROM chat_sessions`).Scan(&rows); err != nil || rows != int(want) {
				t.Fatal("unexpected metadata effect", rows, err)
			}
		})
	}
}
func TestOriginalInputContractCannotDowngrade(t *testing.T) {
	s := newSessionTestServer(t)
	root := t.TempDir()
	var mode atomic.Int32
	mode.Store(2)
	var calls atomic.Int32
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/projects", func(w http.ResponseWriter, r *http.Request) {
		switch mode.Load() {
		case 2:
			w.Header().Set("X-Aimee-Original-Input", "required")
		case 1:
			w.Header().Set("X-Aimee-Original-Input", "optional")
		}
		fmt.Fprintf(w, `{"root":%q}`, root)
	})
	mux.HandleFunc("/v1/chat/stream", func(w http.ResponseWriter, r *http.Request) { calls.Add(1) })
	s.cfg = startFakeV1(t, mux)
	send := func(code int) {
		body := fmt.Sprintf(`{"aimee_session_id":%q,"message":"Synthetic text","original_input_operation_uuid":%q}`, oiSID, oiUUID)
		r := httptest.NewRequest(http.MethodPost, "/api/chat/send", strings.NewReader(body))
		addWebSession(t, s, r, "alice")
		w := httptest.NewRecorder()
		s.requireAuth(s.handleChatSend)(w, r)
		if w.Code != code {
			t.Errorf("status%d expected%d", w.Code, code)
		}
	}
	send(200)
	mode.Store(1)
	send(503)
	mode.Store(0)
	send(503)
	if calls.Load() != 1 {
		t.Fatal("downgrade dispatched", calls.Load())
	}
	cs, e := s.getChatSession("alice", oiSID)
	if e != nil || cs == nil || cs.Title != "" || cs.Cwd != "" {
		t.Fatal("downgrade persisted metadata", e)
	}
	// A known required contract may be read concurrently without a shared mode rewrite.
	mode.Store(2)
	var wg sync.WaitGroup
	for i := 0; i < 8; i++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			rootResult, strict, e := s.webuserWorkspaceRoot(t.Context(), "alice")
			if e != nil || !strict || rootResult != root {
				t.Error("concurrent readiness", e)
			}
		}()
	}
	wg.Wait()
}
