package egress

import (
	"context"
	"net"
	"net/url"
	"strings"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

type fixedResolver []net.IPAddr

func (r fixedResolver) LookupIPAddr(context.Context, string) ([]net.IPAddr, error) { return r, nil }

const digest = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

func TestHandlerBindsCallerPurposeAndResolvedAddress(t *testing.T) {
	handler := newHandler(fixedResolver{{IP: net.ParseIP("140.82.121.3")}})
	body := []byte(`{"target_url":"https://api.github.com/repos/o/r","purpose":"forge","method":"GET","request_sha256":"` + digest + `","credential_present":true}`)
	reply, status := handler(bus.ModuleInvocation{StageID: StageAuthorize, PrincipalClass: 1,
		PrincipalRef: GitClientRef}, body)
	if status != bus.ModuleStatusOK || !containsAll(string(reply), `"allowed":true`, `"140.82.121.3"`, PolicyRevision) {
		t.Fatalf("status=%d reply=%s", status, reply)
	}

	reply, status = handler(bus.ModuleInvocation{StageID: StageAuthorize, PrincipalClass: 1,
		PrincipalRef: MemoryClientRef}, body)
	if status != bus.ModuleStatusOK || !containsAll(string(reply), `"allowed":false`, "caller is not allowed") {
		t.Fatalf("cross-purpose status=%d reply=%s", status, reply)
	}
}

func TestHandlerDeniesPrivateMCPResolutionButAllowsLocalEmbedder(t *testing.T) {
	handler := newHandler(fixedResolver{{IP: net.ParseIP("127.0.0.1")}})
	mcp := []byte(`{"target_url":"https://mcp.example.test/sse","purpose":"mcp_sse","method":"GET","request_sha256":"` + digest + `"}`)
	reply, _ := handler(bus.ModuleInvocation{StageID: StageAuthorize, PrincipalClass: 1,
		PrincipalRef: 200 + PluginClientOffset}, mcp)
	if !containsAll(string(reply), `"allowed":false`, "non-public") {
		t.Fatalf("private MCP reply=%s", reply)
	}
	embed := []byte(`{"target_url":"http://localhost:8080/embed","purpose":"embedding","method":"POST","request_sha256":"` + digest + `"}`)
	reply, _ = handler(bus.ModuleInvocation{StageID: StageAuthorize, PrincipalClass: 1,
		PrincipalRef: MemoryClientRef}, embed)
	if !containsAll(string(reply), `"allowed":true`, "127.0.0.1") {
		t.Fatalf("local embed reply=%s", reply)
	}
}

func TestCallerPoliciesConstrainMethodAndPath(t *testing.T) {
	cases := []struct {
		name      string
		principal uint32
		purpose   string
		method    string
		target    string
	}{
		{"memory method", MemoryClientRef, "embedding", "GET", "http://localhost/embed"},
		{"memory path", MemoryClientRef, "embedding", "POST", "http://localhost/health"},
		{"forge method", GitClientRef, "forge", "DELETE", "https://api.github.com/repos/o/r"},
		{"forge path", GitClientRef, "forge", "GET", "https://api.github.com/user"},
		{"roundtable method", RoundtableClientRef, "review_artifact", "POST", "https://github.com/o/r.patch"},
		{"mcp method", 200 + PluginClientOffset, "mcp_sse", "POST", "https://mcp.example.test/sse"},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			parsed, err := url.Parse(tc.target)
			if err != nil {
				t.Fatal(err)
			}
			if callerPurposeAllowed(tc.principal, Request{Purpose: tc.purpose, Method: tc.method}, parsed) {
				t.Fatalf("unexpected allow for %s %s", tc.method, tc.target)
			}
		})
	}
}

func containsAll(value string, parts ...string) bool {
	for _, part := range parts {
		if !strings.Contains(value, part) {
			return false
		}
	}
	return true
}
