package main

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync/atomic"
	"testing"
)

func TestAllFrontendSPARoutesAreRegistered(t *testing.T) {
	var s server
	mux := http.NewServeMux()
	s.registerRoutes(mux)
	for _, path := range []string{
		"/chat", "/dashboard", "/logs", "/edit-workflows", "/workflow-actions",
		"/providers", "/models", "/agents", "/delegates", "/personas", "/roles",
		"/roundtable", "/projects", "/graph", "/editor", "/memory", "/settings",
	} {
		req := httptest.NewRequest(http.MethodGet, path, nil)
		_, pattern := mux.Handler(req)
		if pattern != path {
			t.Errorf("GET %s resolved to %q; hard refresh would not serve the SPA", path, pattern)
		}
	}
}

func TestMemoryCenterRoutesAreRegistered(t *testing.T) {
	var s server
	mux := http.NewServeMux()
	s.registerRoutes(mux)
	for _, path := range []string{"/v1/memory/get", "/v1/memory/correction_proposals", "/v1/memory/review_correction", "/v1/memory/delete", "/v1/memory/review", "/v1/memory/reject", "/v1/memory/restore"} {
		req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(`{}`))
		_, pattern := mux.Handler(req)
		if pattern != path {
			t.Errorf("POST %s resolved to %q, want exact Memory Center route", path, pattern)
		}
	}
}

func TestMemoryCenterProxyPreservesActorBodyAndStatus(t *testing.T) {
	var gotMethod, gotPath, gotUser, gotBody string
	upstream := http.NewServeMux()
	upstream.HandleFunc("/v1/memory/reject", func(w http.ResponseWriter, r *http.Request) {
		gotMethod, gotPath, gotUser = r.Method, r.URL.Path, r.Header.Get("X-Aimee-Webuser")
		body, _ := io.ReadAll(r.Body)
		gotBody = string(body)
		w.WriteHeader(http.StatusConflict)
		fmt.Fprint(w, `{"error":"refused"}`)
	})
	s := &server{cfg: startFakeV1(t, upstream)}
	req := withUser(httptest.NewRequest(http.MethodPost, "/v1/memory/reject",
		strings.NewReader(`{"id":42,"reason":"wrong"}`)), "alice")
	req.Header.Set("Origin", "http://example.com")
	rec := httptest.NewRecorder()
	s.memoryProxyHandler("/v1/memory/reject")(rec, req)
	waitFakeV1(t)
	if rec.Code != http.StatusConflict || rec.Body.String() != `{"error":"refused"}` {
		t.Fatalf("response code=%d body=%q", rec.Code, rec.Body.String())
	}
	if gotMethod != http.MethodPost || gotPath != "/v1/memory/reject" || gotUser != "alice" {
		t.Fatalf("forwarded method=%q path=%q user=%q", gotMethod, gotPath, gotUser)
	}
	if gotBody != `{"id":42,"reason":"wrong"}` {
		t.Fatalf("forwarded body=%q", gotBody)
	}
}

func TestMemoryCenterProxyRejectsWrongMethod(t *testing.T) {
	s := &server{}
	rec := httptest.NewRecorder()
	s.memoryProxyHandler("/v1/memory/review")(rec,
		httptest.NewRequest(http.MethodGet, "/v1/memory/review", nil))
	if rec.Code != http.StatusMethodNotAllowed {
		t.Fatalf("GET review=%d, want %d", rec.Code, http.StatusMethodNotAllowed)
	}
}

func TestMemoryDetailAndCorrectionAuthenticatedRoutes(t *testing.T) {
	for _, path := range []string{"/v1/memory/get", "/v1/memory/correction_proposals", "/v1/memory/review_correction"} {
		t.Run(path, func(t *testing.T) {
			const body = `{"store":"user","id":42,"proposal_id":"synthetic-proposal"}`
			const reply = `{"status":"error","kind":"conflict"}`
			var calls atomic.Int32
			upstream := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
				calls.Add(1)
				data, _ := io.ReadAll(r.Body)
				if r.Method != http.MethodPost || r.URL.Path != path || r.Header.Get("X-Aimee-Webuser") != "alice" || string(data) != body {
					t.Errorf("proxy changed fixed route, authenticated actor, method, or body")
				}
				w.WriteHeader(http.StatusConflict)
				fmt.Fprint(w, reply)
			})
			s := newLoginTestServer(t, stubIdentity{password: "synthetic"})
			s.cfg = startFakeV1(t, upstream)
			token, err := s.sessions.CreateSession("alice")
			if err != nil {
				t.Fatal(err)
			}
			mux := http.NewServeMux()
			s.registerRoutes(mux)
			for _, tc := range []struct {
				name, origin  string
				authenticated bool
				status        int
			}{
				{"authenticated same origin", "http://example.com", true, http.StatusConflict},
				{"missing session", "http://example.com", false, http.StatusFound},
				{"cross origin", "https://foreign.example", true, http.StatusForbidden},
				{"missing origin", "", true, http.StatusForbidden},
			} {
				t.Run(tc.name, func(t *testing.T) {
					before := calls.Load()
					req := httptest.NewRequest(http.MethodPost, path, strings.NewReader(body))
					req.Header.Set("Origin", tc.origin)
					req.Header.Set("X-Aimee-Webuser", "forged-other")
					if tc.authenticated {
						req.AddCookie(&http.Cookie{Name: "session", Value: token})
					}
					rec := httptest.NewRecorder()
					mux.ServeHTTP(rec, req)
					if rec.Code != tc.status {
						t.Fatalf("status=%d want=%d", rec.Code, tc.status)
					}
					if tc.status == http.StatusConflict {
						if calls.Load() != before+1 || rec.Body.String() != reply || rec.Header().Get("Content-Type") != "application/json" {
							t.Fatal("response status/body/type or forwarding count changed")
						}
					} else if calls.Load() != before {
						t.Fatal("refused request reached upstream")
					}
				})
			}
			waitFakeV1(t)
		})
	}
}
