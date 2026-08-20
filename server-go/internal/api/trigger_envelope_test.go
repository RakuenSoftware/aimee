package api

// `aimee trigger fire` reaches this route through the generic CLI dispatch,
// whose body always carries the method being invoked and the client protocol
// version. The daemon forwards that body to this module verbatim. The C-served
// routes ignore the extras; this decoder is strict and refused the request
// outright -- "decode trigger: json: unknown field \"method\"" -- so the command
// could not run at all once the route moved behind the module.

import (
	"bytes"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

func TestTriggerFireAcceptsTheCLIDispatchEnvelope(t *testing.T) {
	// The shape this slice supports (proposals/watch-dir), wearing the envelope
	// the CLI always sends.
	body := `{"method":"trigger.fire","protocol_version":1,` +
		`"source":"proposals","proposal":"p.md","workspace":"/tmp"}`
	var request triggerFireRequest
	decoder := jsonDecoder(strings.NewReader(body))
	if err := decoder.Decode(&request); err != nil {
		t.Fatalf("a CLI-shaped body must decode: %v", err)
	}
	if request.Source != "proposals" {
		t.Errorf("source = %q, want \"proposals\"", request.Source)
	}
	if request.Workspace != "/tmp" {
		t.Errorf("workspace = %q, want \"/tmp\"", request.Workspace)
	}
}

// The envelope is the only thing newly tolerated: a misspelled real field is
// still a decode error, which is what the strict decoder is for.
func TestTriggerFireStillRejectsAMisspelledField(t *testing.T) {
	body := `{"method":"trigger.fire","source":"proposals","workspac":"/tmp"}`
	var request triggerFireRequest
	if err := jsonDecoder(strings.NewReader(body)).Decode(&request); err == nil {
		t.Error("a misspelled field must still be refused")
	}
}

// End to end through the handler: the request that used to 400 now gets past
// decoding. Whatever it does next, it must not fail on the envelope.
func TestTriggerFireRequestGetsPastDecoding(t *testing.T) {
	server, _, _ := newTestServer(t)
	payload, err := json.Marshal(map[string]any{
		"method": "trigger.fire", "protocol_version": 1,
		"source": "proposals", "proposal": "p.md", "workspace": t.TempDir(),
	})
	if err != nil {
		t.Fatal(err)
	}
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(payload)))
	if strings.Contains(rec.Body.String(), "unknown field") {
		t.Errorf("still refusing the envelope: %s", rec.Body.String())
	}
}

// An unsupported source must be refused BY THE SOURCE CHECK, which knows what
// this slice serves, not by the decoder complaining about a field name. The CLI
// documents `--source manual --task ...`; this slice does not serve it, and the
// operator needs to be told that rather than "unknown field \"task\"".
func TestAnUnsupportedSourceIsRefusedByName(t *testing.T) {
	server, _, _ := newTestServer(t)
	payload, err := json.Marshal(map[string]any{
		"method": "trigger.fire", "protocol_version": 1,
		"source": "manual", "task": "check the build", "workspace": t.TempDir(),
	})
	if err != nil {
		t.Fatal(err)
	}
	rec := httptest.NewRecorder()
	server.ServeHTTP(rec, httptest.NewRequest(http.MethodPost, "/v1/trigger/fire", bytes.NewReader(payload)))
	body := rec.Body.String()
	if strings.Contains(body, "unknown field") {
		t.Errorf("refused by the decoder rather than the source check: %s", body)
	}
	if !strings.Contains(body, "proposals/watch-dir") {
		t.Errorf("the refusal must name what this slice serves, got: %s", body)
	}
}
