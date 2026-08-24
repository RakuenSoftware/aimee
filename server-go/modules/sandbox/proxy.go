package sandbox

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net"
	"net/url"
	"strconv"
	"strings"
	"sync"
	"sync/atomic"
	"time"
)

const (
	ProxyDeadline  = 10 * time.Minute
	ProxyByteLimit = int64(2 * 1024 * 1024 * 1024)
	ProxyHeadLimit = 64 * 1024
)

// Proxy owns the delegate's only network-capable path. Lookup and DialContext
// are injectable so the policy and DNS-pinning behavior are testable without
// public network access.
type Proxy struct {
	// Test seams are package-private so production callers cannot supply a
	// wider allowlist, a lying resolver, or a dialer that ignores the validated
	// numeric address.
	allowlist   string
	lookupIP    func(context.Context, string) ([]net.IPAddr, error)
	dialContext func(context.Context, string, string) (net.Conn, error)
}

type proxyRequest struct {
	method, host, origin, version string
	port                          int
	connect                       bool
	head, remainder               []byte
}

// ProxyDestination is the auditable destination the module actually reached.
// IP is the validated numeric address passed to the dialer, not a later DNS
// observation.
type ProxyDestination struct {
	Host string
	Port int
	IP   net.IP
}

var strippedProxyHeaders = map[string]bool{
	"authorization": true, "proxy-authorization": true, "cookie": true,
	"connection": true, "proxy-connection": true, "keep-alive": true,
	"transfer-encoding": true, "te": true, "trailer": true, "upgrade": true,
}

func splitProxyHead(raw []byte) ([]byte, []byte, error) {
	if len(raw) > ProxyHeadLimit {
		return nil, nil, fmt.Errorf("proxy request head exceeds %d bytes", ProxyHeadLimit)
	}
	marker := []byte("\r\n\r\n")
	idx := bytes.Index(raw, marker)
	if idx < 0 {
		return nil, nil, errors.New("unterminated proxy request head")
	}
	return raw[:idx+len(marker)], raw[idx+len(marker):], nil
}

func parseProxyRequest(raw []byte) (proxyRequest, error) {
	var req proxyRequest
	head, remainder, err := splitProxyHead(raw)
	if err != nil {
		return req, err
	}
	lines := strings.Split(string(head[:len(head)-4]), "\r\n")
	if len(lines) == 0 {
		return req, errors.New("missing proxy request line")
	}
	parts := strings.Split(lines[0], " ")
	if len(parts) != 3 || parts[0] == "" ||
		(parts[2] != "HTTP/1.0" && parts[2] != "HTTP/1.1") {
		return req, errors.New("invalid proxy request line")
	}
	req.method, req.version, req.remainder = parts[0], parts[2], remainder
	for _, r := range req.method {
		if r <= 0x20 || r >= 0x7f {
			return req, errors.New("invalid proxy method")
		}
	}

	if req.method == "CONNECT" {
		host, portText, err := net.SplitHostPort(parts[1])
		if err != nil || host == "" {
			return req, errors.New("CONNECT requires host:port authority")
		}
		port, err := strconv.Atoi(portText)
		if err != nil || !proxyPortAllowed(port) {
			return req, errors.New("CONNECT port is not allowed")
		}
		req.host, req.port, req.connect = host, port, true
	} else {
		u, err := url.ParseRequestURI(parts[1])
		if err != nil || u.Scheme != "http" || u.Hostname() == "" || u.User != nil {
			return req, errors.New("proxy accepts only absolute http URLs without userinfo")
		}
		port := 80
		if u.Port() != "" {
			port, err = strconv.Atoi(u.Port())
			if err != nil {
				return req, errors.New("invalid proxy URL port")
			}
		}
		if !proxyPortAllowed(port) {
			return req, errors.New("proxy URL port is not allowed")
		}
		req.host, req.port, req.origin = u.Hostname(), port, u.RequestURI()
		if req.origin == "" {
			req.origin = "/"
		}
	}

	var rewritten strings.Builder
	if !req.connect {
		fmt.Fprintf(&rewritten, "%s %s %s\r\n", req.method, req.origin, req.version)
		for _, line := range lines[1:] {
			if line == "" || line[0] == ' ' || line[0] == '\t' {
				return req, errors.New("malformed proxy header")
			}
			name, _, found := strings.Cut(line, ":")
			if !found || name == "" || strings.ContainsAny(name, " \t\r\n") {
				return req, errors.New("malformed proxy header")
			}
			if !strippedProxyHeaders[strings.ToLower(name)] {
				rewritten.WriteString(line)
				rewritten.WriteString("\r\n")
			}
		}
		rewritten.WriteString("Connection: close\r\n\r\n")
		req.head = []byte(rewritten.String())
	}
	return req, nil
}

func (p Proxy) lookup(ctx context.Context, host string) ([]net.IPAddr, error) {
	if p.lookupIP != nil {
		return p.lookupIP(ctx, host)
	}
	return net.DefaultResolver.LookupIPAddr(ctx, host)
}

func (p Proxy) dial(ctx context.Context, address string) (net.Conn, error) {
	if p.dialContext != nil {
		return p.dialContext(ctx, "tcp", address)
	}
	return (&net.Dialer{Timeout: 15 * time.Second}).DialContext(ctx, "tcp", address)
}

// dialAllowed resolves once, validates every candidate, and dials the validated
// IP itself. The hostname is never resolved a second time, closing the DNS
// rebinding window between policy and connect.
func (p Proxy) dialAllowed(ctx context.Context, host string, port int) (net.Conn, net.IP, error) {
	allowlist := p.allowlist
	if allowlist == "" {
		allowlist = DefaultPackageAllowlist
	}
	if !proxyHostAllowed(host, allowlist) || !proxyPortAllowed(port) {
		return nil, nil, fmt.Errorf("destination %s:%d is not in the package allowlist", host, port)
	}
	addresses, err := p.lookup(ctx, host)
	if err != nil {
		return nil, nil, fmt.Errorf("resolve %s: %w", host, err)
	}
	if len(addresses) == 0 {
		return nil, nil, fmt.Errorf("resolve %s: no addresses", host)
	}
	var last error
	for _, candidate := range addresses {
		if proxyIPBlocked(candidate.IP) {
			last = fmt.Errorf("resolved address %s is blocked", candidate.IP)
			continue
		}
		address := net.JoinHostPort(candidate.IP.String(), strconv.Itoa(port))
		conn, err := p.dial(ctx, address)
		if err == nil {
			return conn, candidate.IP, nil
		}
		last = err
	}
	if last == nil {
		last = errors.New("no public resolved address")
	}
	return nil, nil, fmt.Errorf("dial %s:%d: %w", host, port, last)
}

type budgetWriter struct {
	w io.Writer
	n *atomic.Int64
}

func (w budgetWriter) Write(p []byte) (int, error) {
	if w.n.Add(int64(len(p))) > ProxyByteLimit {
		return 0, errors.New("proxy byte limit exceeded")
	}
	return w.w.Write(p)
}

func proxyPump(a, b net.Conn) error {
	deadline := time.Now().Add(ProxyDeadline)
	_ = a.SetDeadline(deadline)
	_ = b.SetDeadline(deadline)
	var total atomic.Int64
	errs := make(chan error, 2)
	copyOne := func(dst, src net.Conn) {
		_, err := io.Copy(budgetWriter{w: dst, n: &total}, src)
		if tcp, ok := dst.(*net.TCPConn); ok {
			_ = tcp.CloseWrite()
		}
		errs <- err
	}
	var wg sync.WaitGroup
	wg.Add(2)
	go func() { defer wg.Done(); copyOne(b, a) }()
	go func() { defer wg.Done(); copyOne(a, b) }()
	wg.Wait()
	close(errs)
	for err := range errs {
		if err != nil && !errors.Is(err, net.ErrClosed) {
			return err
		}
	}
	return nil
}

func writeProxyError(client net.Conn, status string) {
	_, _ = io.WriteString(client, "HTTP/1.1 "+status+"\r\nConnection: close\r\n\r\n")
}

// Serve handles one request that arrived over the delegate's Unix control bus.
// It is the only function in the delegate module that can open an IP socket.
func (p Proxy) Serve(ctx context.Context, client net.Conn, rawHead []byte) (ProxyDestination, error) {
	if client == nil {
		return ProxyDestination{}, errors.New("proxy client connection is required")
	}
	req, err := parseProxyRequest(rawHead)
	if err != nil {
		writeProxyError(client, "400 Bad Request")
		return ProxyDestination{}, err
	}
	destination := ProxyDestination{Host: req.host, Port: req.port}
	upstream, ip, err := p.dialAllowed(ctx, req.host, req.port)
	if err != nil {
		writeProxyError(client, "502 Bad Gateway")
		return destination, err
	}
	destination.IP = ip
	defer upstream.Close()
	if req.connect {
		if _, err := io.WriteString(client, "HTTP/1.1 200 Connection Established\r\n\r\n"); err != nil {
			return destination, err
		}
	} else if _, err := upstream.Write(req.head); err != nil {
		return destination, err
	}
	if len(req.remainder) > 0 {
		if _, err := upstream.Write(req.remainder); err != nil {
			return destination, err
		}
	}
	return destination, proxyPump(client, upstream)
}

// ReadProxyHead reads the already-consumed HTTP bytes passed by the C listener.
func ReadProxyHead(r io.Reader) ([]byte, error) {
	reader := bufio.NewReader(io.LimitReader(r, ProxyHeadLimit+1))
	data, err := io.ReadAll(reader)
	if err != nil {
		return nil, err
	}
	if len(data) > ProxyHeadLimit {
		return nil, fmt.Errorf("proxy request exceeds %d bytes", ProxyHeadLimit)
	}
	return data, nil
}
