package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"strings"
	"testing"
)

func TestChatSelectionReadFailureNeverReportsDefault(t *testing.T) {
	for _, kind := range []string{"primary", "persona"} {
		for _, status := range []int{http.StatusForbidden, http.StatusServiceUnavailable} {
			t.Run(fmt.Sprintf("%s/%d", kind, status), func(t *testing.T) {
				mux := http.NewServeMux()
				mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { http.Error(w, "upstream refused", status) })
				s := &server{cfg: startFakeV1(t, mux)}
				result := httptest.NewRecorder()
				request := httptest.NewRequest(http.MethodGet, "/api/chat/"+kind+"?sid=web1", nil)
				if kind == "primary" {
					s.handleChatPrimary(result, request)
				} else {
					s.handleChatPersona(result, request)
				}
				if result.Code != status || !strings.Contains(result.Body.String(), `"error"`) {
					t.Fatalf("failure became selection: %d %s", result.Code, result.Body.String())
				}
			})
		}
	}
}
func TestChatSelectionUnavailableTransportIsAnError(t *testing.T) {
	for _, kind := range []string{"primary", "persona"} {
		s := &server{cfg: &config{socketPath: filepath.Join(t.TempDir(), "missing.sock")}}
		result := httptest.NewRecorder()
		request := httptest.NewRequest(http.MethodGet, "/api/chat/"+kind+"?sid=web1", nil)
		if kind == "primary" {
			s.handleChatPrimary(result, request)
		} else {
			s.handleChatPersona(result, request)
		}
		if result.Code != http.StatusServiceUnavailable {
			t.Fatalf("%s outage became %d %s", kind, result.Code, result.Body.String())
		}
	}
}
func TestChatPrimarySuccessfulUnpinnedReplyStaysDefault(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"agent":""}`) })
	s := &server{cfg: startFakeV1(t, mux)}
	result := httptest.NewRecorder()
	s.handleChatPrimary(result, httptest.NewRequest(http.MethodGet, "/api/chat/primary?sid=web1", nil))
	if result.Code != http.StatusOK || result.Body.String() != `{"agent":""}` {
		t.Fatalf("actual unpinned reply changed: %d %s", result.Code, result.Body.String())
	}
}
func TestChatThreadsDeclaresUnsupportedBranching(t *testing.T) {
	s := &server{cfg: &config{}}
	result := httptest.NewRecorder()
	s.handleChatThreads(result, httptest.NewRequest(http.MethodGet, "/api/chat/threads", nil))
	var body struct {
		Capabilities map[string]bool `json:"capabilities"`
	}
	if err := json.Unmarshal(result.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	supported, present := body.Capabilities["branching"]
	if result.Code != http.StatusOK || !present || supported {
		t.Fatalf("unsupported branching advertised: %s", result.Body.String())
	}
}
