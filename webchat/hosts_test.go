package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"testing"
)

// TestHandleHostsProxiesV1 checks GET /api/hosts forwards to GET /v1/hosts and
// relays the server's {status,hosts:[...]} envelope (host + live GPU inventory).
func TestHandleHostsProxiesV1(t *testing.T) {
	var method, path string
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/hosts", func(w http.ResponseWriter, r *http.Request) {
		method, path = r.Method, r.URL.Path
		fmt.Fprint(w, `{"status":"ok","hosts":[`+
			`{"name":"smoothnas","kind":"remote","ip":"192.168.1.254","gpus":[`+
			`{"index":0,"name":"Radeon RX 7900 XTX","vendor":"amd","vram_mb":24560}]},`+
			`{"name":"local","kind":"local","gpus":[]}]}`)
	})
	s := &server{cfg: startFakeV1(t, mux)}

	rr := httptest.NewRecorder()
	s.handleHosts(rr, httptest.NewRequest(http.MethodGet, "/api/hosts", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if method != http.MethodGet || path != "/v1/hosts" {
		t.Fatalf("forwarded %s %s, want GET /v1/hosts", method, path)
	}
	var out struct {
		Hosts []struct {
			Name string `json:"name"`
			Kind string `json:"kind"`
			GPUs []struct {
				Name   string `json:"name"`
				Vendor string `json:"vendor"`
				VRAMMB int    `json:"vram_mb"`
			} `json:"gpus"`
		} `json:"hosts"`
	}
	if err := json.Unmarshal(rr.Body.Bytes(), &out); err != nil {
		t.Fatalf("decode relay: %v (body=%q)", err, rr.Body.String())
	}
	if len(out.Hosts) != 2 {
		t.Fatalf("hosts=%d want 2", len(out.Hosts))
	}
	if out.Hosts[0].Name != "smoothnas" || len(out.Hosts[0].GPUs) != 1 ||
		out.Hosts[0].GPUs[0].VRAMMB != 24560 || out.Hosts[0].GPUs[0].Vendor != "amd" {
		t.Fatalf("remote host/GPU not relayed: %+v", out.Hosts[0])
	}
}

// TestHandleHostsRejectsNonGET: only GET is allowed.
func TestHandleHostsRejectsNonGET(t *testing.T) {
	s := &server{cfg: &config{}}
	rr := httptest.NewRecorder()
	s.handleHosts(rr, httptest.NewRequest(http.MethodPost, "/api/hosts", nil))
	if rr.Code != http.StatusMethodNotAllowed {
		t.Fatalf("code=%d want 405", rr.Code)
	}
}
