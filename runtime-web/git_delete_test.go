package main

import (
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestGitDeleteRelayReportsOnlyLocalDelete(t *testing.T) {
	recorder := httptest.NewRecorder()
	(&server{}).gitDeleteRelay(recorder, http.StatusOK,
		[]byte(`{"ok":true,"ref":"acme/widget","kb_status":"purged","purge_id":"p1"}`), nil)

	if recorder.Code != http.StatusOK {
		t.Fatalf("status = %d, want 200", recorder.Code)
	}
	var got map[string]any
	if err := json.Unmarshal(recorder.Body.Bytes(), &got); err != nil {
		t.Fatalf("invalid JSON: %v", err)
	}
	if len(got) != 2 || got["ok"] != true || got["ref"] != "acme/widget" {
		t.Fatalf("response = %#v, want exactly local delete fields", got)
	}
}
