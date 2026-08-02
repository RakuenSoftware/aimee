package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"path/filepath"
	"testing"
)

// The banner these states drive is the only thing telling a user that an empty
// search result is a lie. It has to be RIGHT about which dependency is down, or
// it trains people to ignore it. handleReady used to answer every failure with
// kb:"fail" — a specific accusation it had no evidence for.

func readyDeps(t *testing.T, s *server, r *http.Request) (map[string]string, string) {
	t.Helper()
	rr := httptest.NewRecorder()
	s.handleReady(rr, r)
	var got struct {
		Status       string            `json:"status"`
		Dependencies map[string]string `json:"dependencies"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &got); err != nil {
		t.Fatalf("decode %q: %v", rr.Body.String(), err)
	}
	return got.Dependencies, got.Status
}

// The wizard case that made this bug visible: a freshly enrolled instance has no
// webchat session yet, so v1RequestWebuser short-circuits to 401 before it ever
// reaches aimee-server. That is "we could not ask", not "the kb is down".
func TestHandleReadyNoSessionAccusesNothing(t *testing.T) {
	s := &server{cfg: startFakeV1(t, http.NewServeMux())}

	deps, status := readyDeps(t, s, httptest.NewRequest(http.MethodGet, "/api/ready", nil))

	if deps["kb"] == "fail" {
		t.Fatalf("no session reported the kb as failed: %v", deps)
	}
	for _, dep := range []string{"kb", "db1", "retrieval"} {
		if deps[dep] != "unknown" {
			t.Fatalf("dep %q = %q, want unknown: %v", dep, deps[dep], deps)
		}
	}
	if status != "unknown" {
		t.Fatalf("status = %q, want unknown", status)
	}
}

// A server we genuinely cannot reach still has to warn — the user's results are
// untrustworthy — but in terms we can stand behind. We never reached the kb, so
// we cannot report on it.
func TestHandleReadyUnreachableBlamesRetrievalNotKB(t *testing.T) {
	cfg := startFakeV1(t, http.NewServeMux())
	cfg.socketPath = filepath.Join(t.TempDir(), "aimee.sock") // no listener behind it
	s := &server{cfg: cfg}

	deps, status := readyDeps(t, s, withUser(httptest.NewRequest(http.MethodGet, "/api/ready", nil), "admin"))

	if deps["kb"] == "fail" {
		t.Fatalf("unreachable server blamed the kb: %v", deps)
	}
	if deps["retrieval"] != "fail" {
		t.Fatalf("retrieval = %q, want fail: %v", deps["retrieval"], deps)
	}
	if status != "degraded" {
		t.Fatalf("status = %q, want degraded", status)
	}
}

// The real fault on a healthy-kb instance: retrieval down because the embedder
// never loaded. It must survive the relay verbatim, or the banner names the
// wrong thing and the operator restarts the wrong service.
func TestHandleReadyRelaysUpstreamVerbatim(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/ready", func(w http.ResponseWriter, _ *http.Request) {
		w.WriteHeader(http.StatusServiceUnavailable)
		fmt.Fprint(w, `{"ready":false,"status":"degraded",`+
			`"dependencies":{"db1":"ok","kb":"ok","retrieval":"fail"}}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	deps, _ := readyDeps(t, s, withUser(httptest.NewRequest(http.MethodGet, "/api/ready", nil), "admin"))

	// 503 is how aimee-server reports "degraded" — an ANSWER, not a failure to
	// answer — so the upstream verdict must survive intact. Reporting kb:"fail"
	// here sends the operator to restart a knowledge service that is running fine.
	if deps["kb"] != "ok" {
		t.Fatalf("kb = %q, want ok preserved through a 503: %v", deps["kb"], deps)
	}
	if deps["db1"] != "ok" {
		t.Fatalf("db1 = %q, want ok preserved through a 503: %v", deps["db1"], deps)
	}
	if deps["retrieval"] != "fail" {
		t.Fatalf("retrieval = %q, want fail: %v", deps["retrieval"], deps)
	}
}
