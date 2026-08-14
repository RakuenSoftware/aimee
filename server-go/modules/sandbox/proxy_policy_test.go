package sandbox

import (
	"encoding/json"
	"net"
	"testing"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestProxyIPv4Policy(t *testing.T) {
	blocked := []string{
		"127.0.0.1", "127.255.255.255", "0.0.0.0", "10.0.0.1", "10.255.255.255",
		"172.16.0.1", "172.31.255.255", "192.168.1.1", "169.254.1.1",
		"169.254.169.254", "100.64.0.1", "100.127.255.255", "224.0.0.1",
		"239.255.255.255", "240.0.0.1", "255.255.255.255", "192.0.0.1",
		"198.18.0.1", "192.0.2.1", "198.51.100.1", "203.0.113.1",
	}
	for _, address := range blocked {
		if !proxyIPBlocked(net.ParseIP(address)) {
			t.Errorf("%s must be blocked", address)
		}
	}
	public := []string{
		"8.8.8.8", "1.1.1.1", "151.101.0.223", "140.82.112.3",
		"172.15.255.255", "172.32.0.0", "100.63.255.255", "100.128.0.0",
	}
	for _, address := range public {
		if proxyIPBlocked(net.ParseIP(address)) {
			t.Errorf("%s must be allowed", address)
		}
	}
}

func TestProxyIPv6Policy(t *testing.T) {
	blocked := []string{
		"::1", "::", "fe80::1", "fc00::1", "fd12:3456::1", "ff02::1",
		"::ffff:127.0.0.1", "::ffff:169.254.169.254", "::ffff:10.0.0.1",
		"64:ff9b::7f00:1", "64:ff9b::a9fe:a9fe", "2002:7f00:1::",
	}
	for _, address := range blocked {
		if !proxyIPBlocked(net.ParseIP(address)) {
			t.Errorf("%s must be blocked", address)
		}
	}
	public := []string{
		"::ffff:8.8.8.8", "64:ff9b::808:808", "2002:808:808::",
		"2606:4700:4700::1111", "2001:4860:4860::8888",
	}
	for _, address := range public {
		if proxyIPBlocked(net.ParseIP(address)) {
			t.Errorf("%s must be allowed", address)
		}
	}
	if !proxyIPBlocked(nil) || !proxyIPBlocked(net.ParseIP("not-an-ip")) {
		t.Fatal("missing and malformed addresses must fail closed")
	}
}

func TestProxyPortsAndAllowlist(t *testing.T) {
	for _, port := range []int{80, 443} {
		if !proxyPortAllowed(port) {
			t.Errorf("port %d must be allowed", port)
		}
	}
	for _, port := range []int{22, 8080, 0, -1} {
		if proxyPortAllowed(port) {
			t.Errorf("port %d must be denied", port)
		}
	}
	for _, host := range []string{
		"registry.npmjs.org", "pypi.org", "files.pythonhosted.org", "deb.debian.org",
		"security.ubuntu.com", "archive.ubuntu.com", "us.archive.ubuntu.com",
		"a.b.archive.ubuntu.com", "REGISTRY.NPMJS.ORG",
	} {
		if !proxyHostAllowed(host, DefaultPackageAllowlist) {
			t.Errorf("%s must match the default allowlist", host)
		}
	}
	for _, host := range []string{
		"evil.com", "", "archive.ubuntu.com.evil.com", "notarchive.ubuntu.com",
		"registry.npmjs.org.evil.com",
	} {
		if proxyHostAllowed(host, DefaultPackageAllowlist) {
			t.Errorf("%s must not match the default allowlist", host)
		}
	}
	custom := "mirror.internal, *.corp.example"
	if !proxyHostAllowed("mirror.internal", custom) || !proxyHostAllowed("a.corp.example", custom) ||
		proxyHostAllowed("other.example", custom) || proxyHostAllowed("registry.npmjs.org", "") {
		t.Fatal("custom allowlist matching changed")
	}
}

func TestProxyRequestClassification(t *testing.T) {
	tests := []struct {
		line string
		kind int
		host string
		port int
	}{
		{"CONNECT registry.npmjs.org:443 HTTP/1.1", ProxyRequestConnect, "registry.npmjs.org", 443},
		{"CONNECT DEB.Debian.ORG:443 HTTP/1.1", ProxyRequestConnect, "deb.debian.org", 443},
		{"CONNECT [2606:4700::1111]:443 HTTP/1.1", ProxyRequestConnect, "2606:4700::1111", 443},
		{"CONNECT registry.npmjs.org HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"CONNECT h:44x HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"CONNECT h:99999 HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"GET /v1/agents HTTP/1.1", ProxyRequestAPI, "", 0},
		{"GET http://deb.debian.org/x/y HTTP/1.1", ProxyRequestAbsolute, "deb.debian.org", 80},
		{"GET http://mirror:8080/x HTTP/1.1", ProxyRequestAbsolute, "mirror", 8080},
		{"garbage", ProxyRequestInvalid, "", 0},
		{"GET", ProxyRequestInvalid, "", 0},
		{"", ProxyRequestInvalid, "", 0},
		{"GET https://x/y HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"CONNECT user@host:443 HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"GET http://u:p@host/x HTTP/1.1", ProxyRequestInvalid, "", 0},
		{"CONNECT host:443 HTTP/2", ProxyRequestInvalid, "", 0},
		{"CONNECT host:443 HTTP/1.0", ProxyRequestConnect, "host", 443},
		{"CONNECT host:443 HTTP/1.1\r\nHost: host\r\n\r\n", ProxyRequestConnect, "host", 443},
	}
	for _, test := range tests {
		kind, host, port := classifyProxyRequestLine(test.line)
		if kind != test.kind || host != test.host || port != test.port {
			t.Errorf("classify %q = (%d,%q,%d), want (%d,%q,%d)", test.line, kind, host, port,
				test.kind, test.host, test.port)
		}
	}
}

func TestRewriteProxyHead(t *testing.T) {
	head := "GET http://deb.debian.org/pool/pkg.deb HTTP/1.1\r\n" +
		"Host: deb.debian.org\r\nAuthorization: secret\r\nCookie: token\r\n" +
		"X-Hop: remove-even-before-connection\r\nConnection: X-Hop, X-Second\r\n" +
		"X-Second: remove-too\r\nX-Keep: yes\r\n\r\n"
	got, ok := rewriteProxyHead(head)
	if !ok {
		t.Fatal("valid absolute-form request was refused")
	}
	want := "GET /pool/pkg.deb HTTP/1.1\r\nHost: deb.debian.org\r\nX-Keep: yes\r\n" +
		"Connection: close\r\n\r\n"
	if got != want {
		t.Fatalf("rewritten head:\n%q\nwant:\n%q", got, want)
	}
	if _, ok := rewriteProxyHead("GET http://deb.debian.org/x HTTP/1.1\r\nBad Header: x\r\n\r\n"); ok {
		t.Fatal("malformed header name was accepted")
	}
}

func invokeProxy(t *testing.T, handler bus.ModuleHandler, stage uint32, request any) ProxyRequestPolicyResponse {
	t.Helper()
	body, err := json.Marshal(request)
	if err != nil {
		t.Fatal(err)
	}
	response, status := handler(bus.ModuleInvocation{StageID: stage}, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("stage %d status = %v", stage, status)
	}
	var decoded ProxyRequestPolicyResponse
	if err := json.Unmarshal(response, &decoded); err != nil {
		t.Fatal(err)
	}
	return decoded
}

func TestProxyPolicyStages(t *testing.T) {
	handler := NewHandler(nil)
	allowed := invokeProxy(t, handler, StageProxyRequest, ProxyRequestPolicyRequest{
		Line: "CONNECT registry.npmjs.org:443 HTTP/1.1",
	})
	if allowed.Kind != ProxyRequestConnect || allowed.Host != "registry.npmjs.org" ||
		allowed.Port != 443 || !allowed.Allowed {
		t.Fatalf("allowed response = %#v", allowed)
	}
	empty := ""
	denied := invokeProxy(t, handler, StageProxyRequest, ProxyRequestPolicyRequest{
		Line: "CONNECT registry.npmjs.org:443 HTTP/1.1", Allowlist: &empty,
	})
	if denied.Allowed {
		t.Fatalf("empty allowlist allowed request: %#v", denied)
	}
	absolute := invokeProxy(t, handler, StageProxyRequest, ProxyRequestPolicyRequest{
		Line: "GET http://deb.debian.org/x HTTP/1.1\r\nAuthorization: secret\r\n\r\n",
	})
	if !absolute.Allowed || absolute.ForwardHead !=
		"GET /x HTTP/1.1\r\nConnection: close\r\n\r\n" {
		t.Fatalf("absolute response = %#v", absolute)
	}

	body, _ := json.Marshal(ProxyAddressPolicyRequest{IP: "169.254.169.254"})
	response, status := handler(bus.ModuleInvocation{StageID: StageProxyAddress}, body)
	if status != bus.ModuleStatusOK {
		t.Fatalf("address status = %v", status)
	}
	var address ProxyAddressPolicyResponse
	if err := json.Unmarshal(response, &address); err != nil || !address.Blocked {
		t.Fatalf("address response = %s, err=%v", response, err)
	}
}

func TestProxyPolicyStageContractAllocation(t *testing.T) {
	const ordinal = 26
	if want := uint32(4096 + ordinal*256 + int(StageProxyRequest)); EventProxyRequest != want {
		t.Fatalf("EventProxyRequest must be %d, got %d", want, EventProxyRequest)
	}
	if want := uint32(4096 + ordinal*256 + int(StageProxyAddress)); EventProxyAddress != want {
		t.Fatalf("EventProxyAddress must be %d, got %d", want, EventProxyAddress)
	}
}
