package main

import (
	"bytes"
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestChatControlReplyRefusesAmbiguousFramesBeforeRuntime(t *testing.T) {
	calls := 0
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/control-reply", func(w http.ResponseWriter, r *http.Request) {
		calls++
		w.WriteHeader(http.StatusOK)
		w.Write([]byte(`{"status":"ok","queued":true}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	base := `{"session_id":"child","request_id":"child-turn:question","reply":{"decision":"deny"}}`
	cases := []string{
		`{"session_id":"parent","session_id":"child","request_id":"child-turn:question","reply":{"decision":"deny"}}`,
		`{"session_id":"child","request_id":"wrong","request_\u0069d":"child-turn:question","reply":{"decision":"deny"}}`,
		`{"session_id":"child","request_id":"child-turn:question","reply":{"decision":"allow","decision":"deny"}}`,
		base + `{}`,
		`{"session_id":"child\u0000foreign","request_id":"child-turn:question","reply":{"decision":"deny"}}`,
		`{"session_id":"child","request_id":"child-turn:question","reply":{"decision":"allow","answers":{"answer":["a\u0000b"]}}}`,
		`{"session_id":"child","request_id":"child-turn:question","reply":{"decision":"allow","answers":{"answer":["\ud800"]}}}`,
		strings.Replace(base, "child", "\xff", 1),
		base + strings.Repeat(" ", 128<<10),
	}
	for index, raw := range cases {
		request := withUser(httptest.NewRequest(http.MethodPost, "/api/chat/control-reply", strings.NewReader(raw)), "alice")
		request.Header.Set("Origin", "http://example.com")
		response := httptest.NewRecorder()
		s.handleChatControlReply(response, request)
		if response.Code != http.StatusBadRequest {
			t.Errorf("case %d: status %d want400", index, response.Code)
		}
	}
	if calls != 0 {
		t.Fatalf("ambiguous frames reached runtime: %d", calls)
	}
}

func TestChatControlReplyPreservesValidatedOriginalBytesAndChildBinding(t *testing.T) {
	raw := []byte(" {\"session_id\":\"child\", \"request_id\":\"child-turn:form\", \"reply\":{\"decision\":\"allow\",\"content\":{\"count\":9007199254740993,\"ratio\":1e-07,\"name\":\"火\"}}}\n")
	calls := 0
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/chat/control-reply", func(w http.ResponseWriter, r *http.Request) {
		calls++
		body, err := io.ReadAll(r.Body)
		if err != nil || !bytes.Equal(body, raw) || r.Header.Get("X-Aimee-Webuser") != "alice" || r.Header.Get("Authorization") != "" {
			t.Errorf("validated frame/authority changed: %s err=%v", body, err)
		}
		w.Write([]byte(`{"status":"ok","queued":true}`))
	})
	s := &server{cfg: startFakeV1(t, mux)}
	request := withUser(httptest.NewRequest(http.MethodPost, "/api/chat/control-reply", bytes.NewReader(raw)), "alice")
	request.Header.Set("Origin", "http://example.com")
	response := httptest.NewRecorder()
	s.handleChatControlReply(response, request)
	waitFakeV1(t)
	if response.Code != http.StatusOK || calls != 1 {
		t.Fatalf("valid child reply failed: status=%d calls=%d body=%s", response.Code, calls, response.Body.String())
	}
}

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
		waitFakeV1(t)
		if response.Code != tc.want {
			t.Fatalf("reply status %d want %d: %s", response.Code, tc.want, response.Body.String())
		}
	}
	if calls != 1 {
		t.Fatalf("cross-origin/spoofed replies reached runtime: %d", calls)
	}
}
