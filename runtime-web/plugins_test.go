package main

import (
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestHandlePluginsListReportsUnavailableRoute(t *testing.T) {
	s := &server{cfg: startFakeV1(t, http.NewServeMux())}
	rr := httptest.NewRecorder()

	s.handlePluginsList(rr, httptest.NewRequest(http.MethodGet, "/api/plugins", nil))

	if rr.Code != http.StatusNotFound {
		t.Fatalf("code=%d want %d, body=%q", rr.Code, http.StatusNotFound, rr.Body.String())
	}
	if !strings.Contains(rr.Body.String(), "plugin loader is not available") {
		t.Fatalf("missing capability error: %q", rr.Body.String())
	}
}

func TestHandlePluginsListRelaysAvailableRoute(t *testing.T) {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/plugins", func(w http.ResponseWriter, r *http.Request) {
		fmt.Fprint(w, `{"data":[{"name":"example","enabled":true}]}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}
	rr := httptest.NewRecorder()

	s.handlePluginsList(rr, httptest.NewRequest(http.MethodGet, "/api/plugins", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if !strings.Contains(rr.Body.String(), `"name":"example"`) {
		t.Fatalf("plugin response not relayed: %q", rr.Body.String())
	}
}
