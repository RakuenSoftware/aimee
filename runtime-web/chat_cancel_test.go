package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestChatCancelExactOwnerProxy(t *testing.T) {
	calls := 0
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/graceful_cancel", func(w http.ResponseWriter, r *http.Request) {
		calls++
		if r.Header.Get("X-Aimee-Webuser") != "alice" {
			t.Errorf("wrong actor")
		}
		b, _ := io.ReadAll(r.Body)
		if string(b) != `{"session_id":"s","turn_id":"t"}` {
			t.Errorf("changed body %s", b)
		}
		w.WriteHeader(409)
		w.Write([]byte(`{"error":"stale"}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	invalid := []string{`{"session_id":"s","turn_id":"t","turn_id":"other"}`, `{"session_id":"s","turn_id":"t"}{}`, `{"session_id":"s","turn_id":"t","message":"autoqueue"}`, `{"session_id":"s","turn_id":null}`, `{"session_id":"s\u0000bad","turn_id":"t"}`, `{"session_id":"s","turn_id":"t"}` + strings.Repeat(" ", 4096)}
	for _, raw := range invalid {
		r := withUser(httptest.NewRequest("POST", "/api/chat/cancel", strings.NewReader(raw)), "alice")
		r.Header.Set("Origin", "http://example.com")
		w := httptest.NewRecorder()
		s.handleChatCancel(w, r)
		if w.Code != 400 {
			t.Errorf("invalid status%d", w.Code)
		}
	}
	if calls != 0 {
		t.Fatal("invalid cancellation reached runtime")
	}
	r := withUser(httptest.NewRequest("POST", "/api/chat/cancel", strings.NewReader(`{"session_id":"s","turn_id":"t"}`)), "alice")
	r.Header.Set("Origin", "http://example.com")
	r.Header.Set("X-Aimee-Webuser", "mallory")
	w := httptest.NewRecorder()
	s.handleChatCancel(w, r)
	if calls != 1 || w.Code != 409 || !strings.Contains(w.Body.String(), "stale") {
		t.Fatalf("route status%d calls%d", w.Code, calls)
	}
	r = withUser(httptest.NewRequest("POST", "/api/chat/cancel", strings.NewReader(`{"session_id":"s","turn_id":"t"}`)), "alice")
	r.Header.Set("Origin", "http://foreign.example")
	w = httptest.NewRecorder()
	s.handleChatCancel(w, r)
	if w.Code != 403 || calls != 1 {
		t.Fatal("foreign origin admitted")
	}
}
