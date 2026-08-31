package sandbox

import (
	"context"
	"io"
	"net"
	"strings"
	"testing"
	"time"
)

func TestParseProxyRequestStripsCredentialsAndPreservesBufferedBody(t *testing.T) {
	req, err := parseProxyRequest([]byte("POST http://deb.debian.org/pkg HTTP/1.1\r\n" +
		"Host: deb.debian.org\r\nAuthorization: secret\r\nCookie: secret\r\n" +
		"X-Trace: ok\r\n\r\nbody"))
	if err != nil {
		t.Fatal(err)
	}
	got := string(req.head)
	if !strings.HasPrefix(got, "POST /pkg HTTP/1.1\r\n") ||
		strings.Contains(strings.ToLower(got), "authorization:") ||
		strings.Contains(strings.ToLower(got), "cookie:") ||
		!strings.Contains(got, "X-Trace: ok\r\n") {
		t.Fatalf("unsafe rewritten head:\n%s", got)
	}
	if string(req.remainder) != "body" {
		t.Fatalf("buffered body lost: %q", req.remainder)
	}
}

func TestProxyDialsTheValidatedResolvedIP(t *testing.T) {
	var dialed string
	left, right := net.Pipe()
	defer left.Close()
	defer right.Close()
	p := Proxy{
		lookupIP: func(context.Context, string) ([]net.IPAddr, error) {
			return []net.IPAddr{{IP: net.ParseIP("1.1.1.1")}}, nil
		},
		dialContext: func(_ context.Context, network, address string) (net.Conn, error) {
			dialed = network + ":" + address
			return left, nil
		},
	}
	conn, _, err := p.dialAllowed(context.Background(), "pypi.org", 443)
	if err != nil {
		t.Fatal(err)
	}
	_ = conn.Close()
	if dialed != "tcp:1.1.1.1:443" {
		t.Fatalf("policy checked one address but dialed %q", dialed)
	}
}

func TestProxyConnectTunnelOverSoleBus(t *testing.T) {
	moduleClient, delegateClient := net.Pipe()
	moduleUpstream, registry := net.Pipe()
	p := Proxy{
		lookupIP: func(context.Context, string) ([]net.IPAddr, error) {
			return []net.IPAddr{{IP: net.ParseIP("1.1.1.1")}}, nil
		},
		dialContext: func(context.Context, string, string) (net.Conn, error) {
			return moduleUpstream, nil
		},
	}
	done := make(chan error, 1)
	go func() {
		_, err := p.Serve(context.Background(), moduleClient,
			[]byte("CONNECT pypi.org:443 HTTP/1.1\r\nHost: pypi.org\r\n\r\n"))
		done <- err
	}()

	response := make([]byte, len("HTTP/1.1 200 Connection Established\r\n\r\n"))
	if _, err := io.ReadFull(delegateClient, response); err != nil {
		t.Fatal(err)
	}
	if string(response) != "HTTP/1.1 200 Connection Established\r\n\r\n" {
		t.Fatalf("unexpected CONNECT response %q", response)
	}
	go func() { _, _ = delegateClient.Write([]byte("hello")) }()
	fromDelegate := make([]byte, 5)
	if _, err := io.ReadFull(registry, fromDelegate); err != nil || string(fromDelegate) != "hello" {
		t.Fatalf("delegate-to-registry tunnel failed: %q %v", fromDelegate, err)
	}
	go func() { _, _ = registry.Write([]byte("world")) }()
	fromRegistry := make([]byte, 5)
	if _, err := io.ReadFull(delegateClient, fromRegistry); err != nil || string(fromRegistry) != "world" {
		t.Fatalf("registry-to-delegate tunnel failed: %q %v", fromRegistry, err)
	}
	_ = delegateClient.Close()
	_ = registry.Close()
	select {
	case err := <-done:
		if err != nil {
			t.Fatal(err)
		}
	case <-time.After(time.Second):
		t.Fatal("proxy tunnel did not terminate")
	}
}

func TestProxyRefusesOffAllowlistBeforeDial(t *testing.T) {
	moduleClient, delegateClient := net.Pipe()
	defer moduleClient.Close()
	defer delegateClient.Close()
	dialed := false
	p := Proxy{dialContext: func(context.Context, string, string) (net.Conn, error) {
		dialed = true
		return nil, nil
	}}
	done := make(chan error, 1)
	go func() {
		_, err := p.Serve(context.Background(), moduleClient,
			[]byte("CONNECT example.com:443 HTTP/1.1\r\n\r\n"))
		done <- err
	}()
	response := make([]byte, len("HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n"))
	if _, err := io.ReadFull(delegateClient, response); err != nil {
		t.Fatal(err)
	}
	if err := <-done; err == nil || dialed {
		t.Fatalf("off-allowlist request reached dialer: err=%v dialed=%v", err, dialed)
	}
}
