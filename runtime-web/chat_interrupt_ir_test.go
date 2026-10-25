package main

import (
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestChatInterruptCanonicalMessage(t *testing.T) {
	message := `{"role":"user","blocks":[{"type":"text","text":"first"},{"type":"image","media_type":"image/png","media_ref":"https://example.invalid/p.png"}]}`
	calls := 0
	cfg := startFakeV1(t, http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		calls++
		body, _ := io.ReadAll(r.Body)
		if r.URL.Path != "/v1/chat/interrupt" || r.Header.Get("X-Aimee-Webuser") != "alice" || !strings.Contains(string(body), `"message":`+message) {
			t.Errorf("canonical message/actor changed: %s %s %s", r.URL.Path, r.Header.Get("X-Aimee-Webuser"), body)
		}
		w.Write([]byte(`{"interrupted":true}`))
	}))
	s := &server{cfg: cfg}
	for _, body := range []string{`{"aimee_session_id":"session","message":"old text"}`, `{"aimee_session_id":"session","message":{"role":"assistant","blocks":[]}}`, `{"aimee_session_id":"session","aimee_session_id":"other","message":` + message + `}`} {
		res := httptest.NewRecorder()
		s.handleChatInterrupt(res, withUser(httptest.NewRequest("POST", "/", strings.NewReader(body)), "alice"))
		if res.Code != 400 {
			t.Fatalf("accepted invalid steer: %d %s", res.Code, res.Body.String())
		}
	}
	if calls != 0 {
		t.Fatal("invalid steering reached transport")
	}
	res := httptest.NewRecorder()
	s.handleChatInterrupt(res, withUser(httptest.NewRequest("POST", "/", strings.NewReader(`{"aimee_session_id":"session","message":`+message+`}`)), "alice"))
	if res.Code != 200 || calls != 1 {
		t.Fatalf("canonical steering failed: %d %s", res.Code, res.Body.String())
	}
}
