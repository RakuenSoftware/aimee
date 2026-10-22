package main

import (
	"encoding/json"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestGitSessionDirPreservesEditorFolderAndActor(t *testing.T) {
	const folder = "/workspace/alice/acme/widget-session-web1"
	var actor, method string
	var body map[string]string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/session-dir", func(w http.ResponseWriter, r *http.Request) {
		actor, method = r.Header.Get("X-Aimee-Webuser"), r.Method
		if err := json.NewDecoder(r.Body).Decode(&body); err != nil {
			t.Error(err)
		}
		io.WriteString(w, `{"dir":"`+folder+`","token":"never-relay","port":4321,"output":"private"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}
	req := withUser(httptest.NewRequest(http.MethodPost, "/api/git/session-dir",
		strings.NewReader(`{"project":"acme/widget","session_id":"web1"}`)), "alice")
	rr := httptest.NewRecorder()
	s.handleGitSessionDir(rr, req)
	if rr.Code != http.StatusOK {
		t.Fatalf("status=%d body=%q", rr.Code, rr.Body.String())
	}
	var got map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if got["dir"] != folder || got["ok"] != true || len(got) != 2 {
		t.Fatalf("editor response=%#v, want only resolved dir and ok", got)
	}
	if actor != "alice" || method != http.MethodPost || len(body) != 2 || body["project"] != "acme/widget" || body["session_id"] != "web1" {
		t.Fatalf("forwarded actor=%q method=%q body=%#v", actor, method, body)
	}
}

func TestGitSessionDirRefusesInvalidOwnerReply(t *testing.T) {
	for _, body := range []string{`{}`, `{"dir":null}`, `{"dir":123}`, `{"dir":""}`, `not json`} {
		t.Run(body, func(t *testing.T) {
			mux := http.NewServeMux()
			mux.HandleFunc("/v1/workspace/session-dir", func(w http.ResponseWriter, r *http.Request) { io.WriteString(w, body) })
			s := &server{cfg: startFakeV1(t, mux)}
			rr := httptest.NewRecorder()
			s.handleGitSessionDir(rr, withUser(httptest.NewRequest(http.MethodPost, "/api/git/session-dir", strings.NewReader(`{"project":"acme/widget"}`)), "alice"))
			if rr.Code != http.StatusBadGateway || strings.Contains(rr.Body.String(), body) {
				t.Fatalf("invalid owner reply status=%d body=%q", rr.Code, rr.Body.String())
			}
		})
	}
}

func TestGitSessionDirPreservesOwnerRefusalWithoutDirectory(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/workspace/session-dir", func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusForbidden)
		io.WriteString(w, `{"error":"session belongs to another user","dir":"/private/other-user","token":"never-relay"}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}
	rr := httptest.NewRecorder()
	s.handleGitSessionDir(rr, withUser(httptest.NewRequest(http.MethodPost, "/api/git/session-dir", strings.NewReader(`{"project":"acme/widget","session_id":"other"}`)), "alice"))
	if rr.Code != http.StatusForbidden || strings.Contains(rr.Body.String(), "/private/") || strings.Contains(rr.Body.String(), "never-relay") {
		t.Fatalf("owner refusal status=%d body=%q", rr.Code, rr.Body.String())
	}
}

func TestGitRelayDoesNotExposeEditorDirectory(t *testing.T) {
	rr := httptest.NewRecorder()
	(&server{}).gitRelay(rr, http.StatusOK, []byte(`{"ok":true,"dir":"/private/unrelated","token":"never-relay"}`), nil)
	var got map[string]any
	if err := json.Unmarshal(rr.Body.Bytes(), &got); err != nil {
		t.Fatal(err)
	}
	if _, present := got["dir"]; present || strings.Contains(rr.Body.String(), "never-relay") {
		t.Fatalf("unrelated git relay broadened exposure: %q", rr.Body.String())
	}
}
