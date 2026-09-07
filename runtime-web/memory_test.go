package main

import (
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
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
	for _, path := range []string{"/v1/memory/delete", "/v1/memory/review", "/v1/memory/reject", "/v1/memory/restore"} {
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
	rec := httptest.NewRecorder()
	s.memoryProxyHandler("/v1/memory/reject")(rec, req)
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
