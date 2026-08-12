package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
)

// The model roster is served under "/api/models" and, for GUI builds from
// before the rename, under the old "/api/agents". Both come from one
// registerModelRoutes call per prefix, so this asserts the alias really covers
// the WHOLE surface — a route added to the canonical set but missing from the
// alias (or vice versa) fails here rather than 404-ing an older browser.
func TestModelRoutesServedUnderBothPrefixes(t *testing.T) {
	ops := []struct {
		method string
		suffix string
	}{
		{http.MethodGet, ""},
		{http.MethodGet, "/stats"},
		{http.MethodPost, "/add"},
		{http.MethodPost, "/remove"},
		{http.MethodPost, "/enable"},
		{http.MethodPost, "/disable"},
		{http.MethodPost, "/probe"},
		{http.MethodPost, "/roles"},
		{http.MethodPost, "/personas"},
		{http.MethodPost, "/set"},
		{http.MethodPost, "/oauth/start"},
		{http.MethodPost, "/oauth/code"},
		{http.MethodPost, "/oauth/poll"},
	}

	s := &server{}
	mux := http.NewServeMux()
	s.registerModelRoutes(mux, "/api/models")
	s.registerModelRoutes(mux, "/api/agents")

	for _, prefix := range []string{"/api/models", "/api/agents"} {
		for _, op := range ops {
			path := prefix + op.suffix
			req := httptest.NewRequest(op.method, path, nil)
			if _, pattern := mux.Handler(req); pattern == "" {
				t.Errorf("%s %s is not routed", op.method, path)
			}
		}
	}
}

// The roster list answers with the same array under BOTH `models` (current) and
// `agents` (pre-rename), so neither an old nor a new GUI build has to know which
// server it is talking to.
func TestHandleModelsEmitsBothRosterKeys(t *testing.T) {
	v1 := http.NewServeMux()
	v1.HandleFunc("/v1/model/list", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"status":"ok","models":[{"name":"gpt"},{"name":"claude"}]}`)
	})
	s := &server{cfg: startFakeV1(t, v1)}

	rr := httptest.NewRecorder()
	s.handleModels(rr, httptest.NewRequest(http.MethodGet, "/api/models", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	var out struct {
		Models []struct{ Name string } `json:"models"`
		Agents []struct{ Name string } `json:"agents"`
		Count  int                     `json:"count"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode: %v (body %q)", err, rr.Body.String())
	}
	if len(out.Models) != 2 || len(out.Agents) != 2 {
		t.Fatalf("want 2 rows under both keys, got models=%d agents=%d", len(out.Models), len(out.Agents))
	}
	if out.Models[0].Name != "gpt" || out.Agents[0].Name != "gpt" {
		t.Fatalf("roster contents differ between keys: %+v", out)
	}
	if out.Count != 2 {
		t.Fatalf("count=%d want 2", out.Count)
	}
}

// A server older than the rename answers model.list under `agents` only. The proxy
// must still surface that roster rather than rendering an empty Models tab.
func TestHandleModelsAcceptsLegacyAgentsKey(t *testing.T) {
	v1 := http.NewServeMux()
	v1.HandleFunc("/v1/model/list", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"status":"ok","agents":[{"name":"legacy"}]}`)
	})
	s := &server{cfg: startFakeV1(t, v1)}

	rr := httptest.NewRecorder()
	s.handleModels(rr, httptest.NewRequest(http.MethodGet, "/api/models", nil))

	var out struct {
		Models []struct{ Name string } `json:"models"`
		Count  int                     `json:"count"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode: %v (body %q)", err, rr.Body.String())
	}
	if len(out.Models) != 1 || out.Models[0].Name != "legacy" {
		t.Fatalf("legacy `agents` roster not surfaced: %+v", out)
	}
}
