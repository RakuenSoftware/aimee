package main

import (
	"encoding/json"
	"net/http"
	"strings"
	"testing"
)

// A dispatch error the server classified must map onto the right HTTP status.
//
// Everything used to become 502 Bad Gateway, so `agent add` with no arguments
// answered "502: usage: agent add <name> <endpoint> <model>" — a caller's
// mistake reported as an upstream failure, which misleads log readers and
// invites clients to retry a request that can never succeed.
func TestRPCErrorStatusMapsClassifiedFaults(t *testing.T) {
	mk := func(body string) error {
		var msg map[string]json.RawMessage
		if err := json.Unmarshal([]byte(body), &msg); err != nil {
			t.Fatalf("bad fixture %s: %v", body, err)
		}
		return rpcError(msg)
	}

	cases := []struct {
		name string
		body string
		want int
	}{
		{"invalid_argument", `{"status":"error","kind":"invalid_argument","http_status":400,"message":"usage: agent add"}`, http.StatusBadRequest},
		{"not_found", `{"status":"error","kind":"not_found","http_status":404,"message":"no such agent"}`, http.StatusNotFound},
		{"permission_denied", `{"status":"error","kind":"permission_denied","http_status":403,"message":"nope"}`, http.StatusForbidden},
		{"unavailable", `{"status":"error","kind":"unavailable","http_status":503,"message":"kb down"}`, http.StatusServiceUnavailable},
		// The module classifies unclassified and unknown faults as 502 without
		// asking the physical web provider to interpret their kind.
		{"unclassified stays 502", `{"status":"error","http_status":502,"message":"something broke"}`, http.StatusBadGateway},
		{"unknown kind stays 502", `{"status":"error","kind":"wat","http_status":502,"message":"x"}`, http.StatusBadGateway},
		// Missing or invalid module output is a transport failure, also 502.
		{"kind without module status stays 502", `{"status":"error","kind":"invalid_argument","message":"x"}`, http.StatusBadGateway},
		{"invalid module status stays 502", `{"status":"error","kind":"invalid_argument","http_status":200,"message":"x"}`, http.StatusBadGateway},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			err := mk(tc.body)
			if err == nil {
				t.Fatalf("expected an error for %s", tc.body)
			}
			if got := rpcErrorStatus(err); got != tc.want {
				t.Fatalf("status = %d, want %d (%s)", got, tc.want, tc.body)
			}
			// The message must survive classification unchanged.
			if !strings.Contains(err.Error(), "server: ") {
				t.Fatalf("message lost its prefix: %q", err.Error())
			}
		})
	}

	// A success envelope is still not an error.
	if err := mk(`{"status":"ok"}`); err != nil {
		t.Fatalf("ok envelope produced an error: %v", err)
	}
}
