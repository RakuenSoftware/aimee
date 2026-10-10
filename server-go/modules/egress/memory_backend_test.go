package egress

import (
	"context"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"testing"
)

func TestMemoryBackendEgressUsesConfiguredOriginAndVault(t *testing.T) {
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/api/v1/datasets" || r.Header.Get("Authorization") != "Bearer fixture-memory-token" {
			t.Error("wrong backend request or credential")
			http.Error(w, "denied", 403)
			return
		}
		w.Write([]byte(`[]`))
	}))
	defer upstream.Close()
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", upstream.URL)
	resolver := &vaultCredentialResolver{run: func(_ context.Context, name string) ([]byte, error) {
		if name != "AIMEE_MEMORY_BACKEND_TOKEN" {
			t.Fatal("backend could read unrelated credential", name)
		}
		return []byte("fixture-memory-token"), nil
	}}
	p := policy{resolver: fixedResolver{}, backendCredentials: resolver}
	// Existing fixedResolver uses a public fixture IP; use loopback resolution for
	// this explicit configured service, under the same policy admission.
	p.resolver = fixedResolver{{IP: net.ParseIP("127.0.0.1")}}
	target := upstream.URL + "/api/v1/datasets"
	request := HTTPRequest{Request: Request{TargetURL: target, Purpose: "memory-backend", Method: "GET", CredentialPresent: true, RequestSHA256: RequestDigest("GET", target, nil, true)}, CredentialHandle: "memory-backend", MaxResponseBytes: 1024, TimeoutMS: 1000}
	raw, _ := json.Marshal(request)
	reply, status := p.handleHTTP(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef}, raw)
	if status != bus.ModuleStatusOK {
		t.Fatal(status)
	}
	out, err := decodeHTTPResponse(reply)
	if err != nil || out.Status != 200 || string(out.Body) != "[]" {
		t.Fatal(out, err)
	}
	for _, bad := range []string{upstream.URL + "/api/v1/datasets/", upstream.URL + "/api/v1/datasets?all=true", "http://other.invalid/api/v1/search", upstream.URL + "/api/v1/prune", upstream.URL + "/api/v1/datasets/../../admin"} {
		u, _ := url.Parse(bad)
		if callerPurposeAllowed(MemoryClientRef, Request{Purpose: "memory-backend", Method: "POST"}, u) {
			t.Fatal("unconfigured or administrative target admitted", bad)
		}
	}
	if secret, err := resolver.Resolve(context.Background(), ProvidersClientRef, "memory-backend"); err == nil {
		clear(secret)
		t.Fatal("foreign module read backend credential")
	}
	if secret, err := resolver.Resolve(context.Background(), MemoryClientRef, "provider"); err == nil {
		clear(secret)
		t.Fatal("backend read provider credential")
	}
}

func TestHillockEgressRoutesAreProviderSpecific(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_BACKEND", "hillock")
	t.Setenv("AIMEE_HILLOCK_URL", "http://127.0.0.1:8097")
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", "")
	for _, tc := range []struct {
		method, path string
		allowed      bool
	}{
		{"GET", "/v1/health", true}, {"POST", "/v1/rank", true},
		{"GET", "/v1/rank", false}, {"POST", "/v1/chat/completions", false},
		{"POST", "/api/v1/add", false}, {"POST", "/v1/rank?all=true", false},
	} {
		target, _ := url.Parse("http://127.0.0.1:8097" + tc.path)
		got := callerPurposeAllowed(MemoryClientRef, Request{Purpose: "memory-backend", Method: tc.method}, target)
		if got != tc.allowed {
			t.Fatalf("%s %s: %v", tc.method, tc.path, got)
		}
		if callerPurposeAllowed(ProvidersClientRef, Request{Purpose: "memory-backend", Method: tc.method}, target) {
			t.Fatal("foreign caller admitted")
		}
	}
	t.Setenv("AIMEE_MEMORY_BACKEND", "cognee")
	if memoryBackendTargetAllowed("POST", "/v1/rank") {
		t.Fatal("Hillock route admitted for Cognee")
	}
}

func TestMemoryBackendAPIKeyIsInjectedOnlyByEgress(t *testing.T) {
	t.Setenv("AIMEE_MEMORY_BACKEND_AUTH", "api-key")
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Header.Get("X-Api-Key") != "service-key" || r.Header.Get("Authorization") != "" {
			t.Error("wrong credential transport")
			w.WriteHeader(403)
			return
		}
		w.Write([]byte(`[]`))
	}))
	defer upstream.Close()
	t.Setenv("AIMEE_MEMORY_BACKEND_URL", upstream.URL)
	resolver := &vaultCredentialResolver{run: func(context.Context, string) ([]byte, error) { return []byte("service-key"), nil }}
	p := policy{resolver: fixedResolver{{IP: net.ParseIP("127.0.0.1")}}, backendCredentials: resolver}
	target := upstream.URL + "/api/v1/datasets"
	request := HTTPRequest{Request: Request{TargetURL: target, Purpose: "memory-backend", Method: "GET", CredentialPresent: true, RequestSHA256: RequestDigest("GET", target, nil, true)}, CredentialHandle: "memory-backend", MaxResponseBytes: 1024, TimeoutMS: 1000}
	raw, _ := json.Marshal(request)
	reply, status := p.handleHTTP(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef}, raw)
	out, err := decodeHTTPResponse(reply)
	if status != bus.ModuleStatusOK || err != nil || out.Status != 200 {
		t.Fatal(status, out, err)
	}
	request.Headers = map[string]string{"X-Api-Key": "untrusted"}
	raw, _ = json.Marshal(request)
	if _, status = p.handleHTTP(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: MemoryClientRef}, raw); status != bus.ModuleStatusInvalidRequest {
		t.Fatal("caller credential header admitted", status)
	}
}
