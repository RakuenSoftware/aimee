package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestDeployRelayNeverCachesEnrollment(t *testing.T) {
	var s server
	w := httptest.NewRecorder()
	s.deployRelay(w, http.StatusOK, []byte(`{"enrollment":{"bearer_token":"secret"}}`), nil)
	if got := w.Header().Get("Cache-Control"); got != "no-store" {
		t.Fatalf("Cache-Control = %q, want no-store", got)
	}
	if w.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", w.Code)
	}
}
