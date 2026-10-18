package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestChatControlReplyForwardsOnlyVerifiedBrowserActor(t *testing.T) {
	calls := 0
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/control-reply", func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Header.Get("X-Aimee-Webuser") != "alice" || r.Header.Get("Authorization") != "" {
			t.Errorf("wrong authority headers")
		}
		raw, _ := io.ReadAll(r.Body)
		var body map[string]any
		if err := json.Unmarshal(raw, &body); err != nil {
			t.Fatal(err)
		}
		if body["session_id"] != "session" || body["request_id"] != "turn:permission" || body["actor"] != nil {
			t.Errorf("wrong body: %s", raw)
		}
		w.Header().Set("Content-Type", "application/json")
		w.WriteHeader(http.StatusConflict)
		w.Write([]byte(`{"error":"stale pending request"}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	for _, tc := range []struct {
		origin, extra string
		want          int
	}{{"http://example.com", "", http.StatusConflict}, {"https://evil.example", "", http.StatusForbidden}, {"http://example.com", `,"actor":"mallory"`, http.StatusBadRequest}} {
		request := withUser(httptest.NewRequest(http.MethodPost, "/api/chat/control-reply", strings.NewReader(`{"session_id":"session","request_id":"turn:permission","reply":{"decision":"allow"}`+tc.extra+`}`)), "alice")
		request.Header.Set("Origin", tc.origin)
		response := httptest.NewRecorder()
		s.handleChatControlReply(response, request)
		if response.Code != tc.want {
			t.Fatalf("reply status %d want %d: %s", response.Code, tc.want, response.Body.String())
		}
	}
	if calls != 1 {
		t.Fatalf("cross-origin/spoofed replies reached runtime: %d", calls)
	}
}
