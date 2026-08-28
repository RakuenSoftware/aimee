package egress

import (
	"bufio"
	"bytes"
	"context"
	"crypto/tls"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

type SSELimits struct {
	MaxLineBytes         int   `json:"max_line_bytes"`
	MaxEventBytes        int   `json:"max_event_bytes"`
	MaxPostResponseBytes int64 `json:"max_post_response_bytes"`
	MaxConnectionBytes   int64 `json:"max_connection_bytes"`
	IdleTimeoutMS        int64 `json:"idle_timeout_ms"`
	ConnectionLifetimeMS int64 `json:"connection_lifetime_ms"`
	PostTimeoutMS        int64 `json:"post_timeout_ms"`
}

func DefaultSSELimits() SSELimits {
	return SSELimits{MaxLineBytes: 64 << 10, MaxEventBytes: 1 << 20,
		MaxPostResponseBytes: 64 << 10, MaxConnectionBytes: 256 << 20,
		IdleTimeoutMS: (2 * time.Minute).Milliseconds(), ConnectionLifetimeMS: (24 * time.Hour).Milliseconds(),
		PostTimeoutMS: (30 * time.Second).Milliseconds()}
}

type SSERequest struct {
	URL              string    `json:"url"`
	CredentialHandle string    `json:"credential_handle,omitempty"`
	TimeoutMS        int64     `json:"timeout_ms"`
	Limits           SSELimits `json:"limits"`
}

type SSEStream interface {
	Send([]byte) error
	Recv(time.Duration) ([]byte, error)
	Close() error
}

type Streamer interface {
	OpenSSE(context.Context, uint64, SSERequest) (SSEStream, error)
}

type streamKey struct {
	principal uint32
	handle    uint64
}

type streamService struct {
	policy      policy
	credentials credentialResolver
	mu          sync.Mutex
	streams     map[streamKey]*networkSSE
	next        atomic.Uint64
}

func newStreamService(policy policy, credentials credentialResolver) *streamService {
	return &streamService{policy: policy, credentials: credentials, streams: make(map[streamKey]*networkSSE)}
}

type streamOpenReply struct {
	Handle uint64 `json:"handle"`
}
type streamCommand struct {
	Handle    uint64 `json:"handle"`
	Frame     []byte `json:"frame,omitempty"`
	TimeoutMS int64  `json:"timeout_ms,omitempty"`
}
type streamRecvReply struct {
	Frame []byte `json:"frame,omitempty"`
	Error string `json:"error,omitempty"`
}

func (s *streamService) handle(invocation bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	if invocation.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	if invocation.StageID == StageSSEOpen {
		var request SSERequest
		if json.Unmarshal(body, &request) != nil || !validSSERequest(request) {
			return nil, bus.ModuleStatusInvalidRequest
		}
		stream, err := s.open(invocation, request)
		if err != nil {
			encoded, _ := json.Marshal(streamRecvReply{Error: err.Error()})
			return encoded, bus.ModuleStatusOK
		}
		handle := s.next.Add(1)
		if handle == 0 {
			handle = s.next.Add(1)
		}
		s.mu.Lock()
		s.streams[streamKey{invocation.PrincipalRef, handle}] = stream
		s.mu.Unlock()
		encoded, _ := json.Marshal(streamOpenReply{Handle: handle})
		return encoded, bus.ModuleStatusOK
	}
	var command streamCommand
	if json.Unmarshal(body, &command) != nil || command.Handle == 0 {
		return nil, bus.ModuleStatusInvalidRequest
	}
	key := streamKey{invocation.PrincipalRef, command.Handle}
	s.mu.Lock()
	stream := s.streams[key]
	if invocation.StageID == StageSSEClose && stream != nil {
		delete(s.streams, key)
	}
	s.mu.Unlock()
	if stream == nil {
		return nil, bus.ModuleStatusInvalidRequest
	}
	switch invocation.StageID {
	case StageSSESend:
		if len(command.Frame) == 0 || len(command.Frame) > stream.limits.MaxEventBytes {
			return nil, bus.ModuleStatusInvalidRequest
		}
		if err := stream.Send(command.Frame); err != nil {
			encoded, _ := json.Marshal(streamRecvReply{Error: err.Error()})
			return encoded, bus.ModuleStatusOK
		}
		return []byte(`{}`), bus.ModuleStatusOK
	case StageSSERecv:
		timeout := time.Duration(command.TimeoutMS) * time.Millisecond
		if timeout <= 0 || timeout > maxHTTPTimeout {
			return nil, bus.ModuleStatusInvalidRequest
		}
		frame, err := stream.Recv(timeout)
		reply := streamRecvReply{Frame: frame}
		if err != nil {
			reply.Error = err.Error()
		}
		encoded, _ := json.Marshal(reply)
		return encoded, bus.ModuleStatusOK
	case StageSSEClose:
		_ = stream.Close()
		return []byte(`{}`), bus.ModuleStatusOK
	default:
		return nil, bus.ModuleStatusInvalidRequest
	}
}

func validSSERequest(request SSERequest) bool {
	l := request.Limits
	return request.URL != "" && validCredentialHandle(request.CredentialHandle) &&
		request.TimeoutMS > 0 && request.TimeoutMS <= maxHTTPTimeout.Milliseconds() &&
		l.MaxLineBytes > 0 && l.MaxLineBytes <= 1<<20 && l.MaxEventBytes > 0 && l.MaxEventBytes <= 4<<20 &&
		l.MaxPostResponseBytes > 0 && l.MaxPostResponseBytes <= 1<<20 &&
		l.MaxConnectionBytes > 0 && l.MaxConnectionBytes <= 1<<30 &&
		l.IdleTimeoutMS > 0 && l.ConnectionLifetimeMS > 0 && l.PostTimeoutMS > 0 &&
		l.PostTimeoutMS <= maxHTTPTimeout.Milliseconds()
}

func validCredentialHandle(handle string) bool {
	if handle == "" {
		return true
	}
	if len(handle) <= len("mcp:") || len(handle) > 32 || !strings.HasPrefix(handle, "mcp:") {
		return false
	}
	for _, char := range handle[len("mcp:"):] {
		if char < '0' || char > '9' {
			return false
		}
	}
	return true
}

func (s *streamService) open(invocation bus.ModuleInvocation, request SSERequest) (*networkSSE, error) {
	var bearer []byte
	if request.CredentialHandle != "" {
		if s.credentials == nil {
			return nil, errors.New("egress: credential handle is unavailable")
		}
		var err error
		ctx, cancel := context.WithTimeout(context.Background(), invocation.Remaining(10*time.Second))
		defer cancel()
		bearer, err = s.credentials.Resolve(ctx, invocation.PrincipalRef, request.CredentialHandle)
		if err != nil {
			return nil, errors.New("egress: credential handle is unavailable")
		}
	}
	credential := len(bearer) != 0
	decision := s.policy.decide(invocation, Request{TargetURL: request.URL, Purpose: "mcp_sse", Method: "GET",
		RequestSHA256: RequestDigest("GET", request.URL, nil, credential), CredentialPresent: credential})
	if !decision.Allowed {
		return nil, errors.New("egress denied: " + decision.Reason)
	}
	stream, err := newNetworkSSE(request, bearer, decision)
	clear(bearer)
	return stream, err
}

type networkSSE struct {
	streamClient *http.Client
	postClient   *http.Client
	baseURL      string
	bearer       []byte
	limits       SSELimits
	body         io.ReadCloser
	endpoint     string
	endpointMu   sync.RWMutex
	frames       chan []byte
	errCh        chan error
	closed       chan struct{}
	closeOnce    sync.Once
}

func pinnedTransport(target string, ips []string, timeout time.Duration) (*http.Transport, error) {
	parsed, err := url.Parse(target)
	if err != nil || parsed.Hostname() == "" || len(ips) == 0 {
		return nil, errors.New("egress: invalid pinned target")
	}
	port := parsed.Port()
	if port == "" {
		if parsed.Scheme == "https" {
			port = "443"
		} else {
			port = "80"
		}
	}
	dialer := &net.Dialer{Timeout: timeout}
	transport := &http.Transport{Proxy: nil, ResponseHeaderTimeout: timeout,
		MaxResponseHeaderBytes: maxHTTPHeaders,
		TLSClientConfig:        &tls.Config{MinVersion: tls.VersionTLS12, ServerName: parsed.Hostname()}}
	transport.DialContext = func(ctx context.Context, network, address string) (net.Conn, error) {
		host, _, err := net.SplitHostPort(address)
		if err != nil || !strings.EqualFold(host, parsed.Hostname()) {
			return nil, errors.New("egress: stream escaped the authorized host")
		}
		var last error
		for _, ip := range ips {
			conn, dialErr := dialer.DialContext(ctx, network, net.JoinHostPort(ip, port))
			if dialErr == nil {
				return conn, nil
			}
			last = dialErr
		}
		return nil, last
	}
	return transport, nil
}

func newNetworkSSE(request SSERequest, bearer []byte, decision Decision) (*networkSSE, error) {
	timeout := time.Duration(request.TimeoutMS) * time.Millisecond
	transport, err := pinnedTransport(decision.Target, decision.ResolvedIPs, timeout)
	if err != nil {
		return nil, err
	}
	postTransport := transport.Clone()
	t := &networkSSE{streamClient: &http.Client{Transport: transport},
		postClient: &http.Client{Transport: postTransport, Timeout: time.Duration(request.Limits.PostTimeoutMS) * time.Millisecond},
		baseURL:    decision.Target, bearer: append([]byte(nil), bearer...), limits: request.Limits,
		frames: make(chan []byte, 16), errCh: make(chan error, 1), closed: make(chan struct{})}
	ctx, cancel := context.WithTimeout(context.Background(), time.Duration(request.Limits.ConnectionLifetimeMS)*time.Millisecond)
	req, err := http.NewRequestWithContext(ctx, "GET", decision.Target, nil)
	if err != nil {
		cancel()
		return nil, err
	}
	req.Header.Set("Accept", "text/event-stream")
	t.auth(req)
	resp, err := t.streamClient.Do(req)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("SSE connect: %w", err)
	}
	if resp.StatusCode/100 != 2 {
		cancel()
		resp.Body.Close()
		return nil, fmt.Errorf("SSE connect: HTTP %d", resp.StatusCode)
	}
	t.body = resp.Body
	endpointCh := make(chan string, 1)
	go t.readLoop(cancel, endpointCh)
	select {
	case endpoint := <-endpointCh:
		t.endpointMu.Lock()
		t.endpoint = endpoint
		t.endpointMu.Unlock()
		return t, nil
	case err := <-t.errCh:
		_ = t.Close()
		return nil, err
	case <-time.After(timeout):
		_ = t.Close()
		return nil, errors.New("no endpoint event before timeout")
	}
}

func (t *networkSSE) auth(req *http.Request) {
	if len(t.bearer) != 0 {
		req.Header.Set("Authorization", "Bearer "+string(t.bearer))
	}
}

func readBoundedLine(reader *bufio.Reader, max int) ([]byte, error) {
	var line []byte
	for {
		part, err := reader.ReadSlice('\n')
		if len(line)+len(part) > max {
			return nil, fmt.Errorf("SSE line exceeds %d bytes", max)
		}
		line = append(line, part...)
		if err == nil {
			return line, nil
		}
		if !errors.Is(err, bufio.ErrBufferFull) {
			return line, err
		}
	}
}

func (t *networkSSE) resolveEndpoint(value string) string {
	base, err := url.Parse(t.baseURL)
	if err != nil {
		return ""
	}
	ref, err := url.Parse(value)
	if err != nil {
		return ""
	}
	resolved := base.ResolveReference(ref)
	if resolved.Scheme != base.Scheme || !strings.EqualFold(resolved.Host, base.Host) {
		return ""
	}
	return resolved.String()
}

func (t *networkSSE) readLoop(cancel context.CancelFunc, endpointCh chan<- string) {
	defer cancel()
	defer close(t.frames)
	reader := bufio.NewReaderSize(t.body, min(t.limits.MaxLineBytes, 64<<10))
	var eventName string
	var data bytes.Buffer
	var connectionBytes int64
	idle := time.AfterFunc(time.Duration(t.limits.IdleTimeoutMS)*time.Millisecond, func() { _ = t.body.Close() })
	defer idle.Stop()
	report := func(err error) {
		select {
		case t.errCh <- fmt.Errorf("SSE stream: %w", err):
		default:
		}
	}
	dispatch := func() error {
		if data.Len() == 0 {
			eventName = ""
			return nil
		}
		payload := strings.TrimRight(data.String(), "\n")
		data.Reset()
		name := eventName
		eventName = ""
		if name == "endpoint" {
			resolved := t.resolveEndpoint(strings.TrimSpace(payload))
			if resolved == "" {
				return errors.New("endpoint must retain the SSE origin")
			}
			select {
			case endpointCh <- resolved:
			default:
			}
			return nil
		}
		select {
		case t.frames <- []byte(payload):
		case <-t.closed:
		}
		return nil
	}
	for {
		lineBytes, err := readBoundedLine(reader, t.limits.MaxLineBytes)
		connectionBytes += int64(len(lineBytes))
		if connectionBytes > t.limits.MaxConnectionBytes {
			report(errors.New("SSE connection byte limit exceeded"))
			return
		}
		if len(lineBytes) > 0 {
			idle.Reset(time.Duration(t.limits.IdleTimeoutMS) * time.Millisecond)
		}
		line := strings.TrimRight(string(lineBytes), "\r\n")
		switch {
		case line == "":
			if dispatchErr := dispatch(); dispatchErr != nil {
				report(dispatchErr)
				return
			}
		case strings.HasPrefix(line, ":"):
		case strings.HasPrefix(line, "event:"):
			eventName = strings.TrimSpace(line[len("event:"):])
		case strings.HasPrefix(line, "data:"):
			value := strings.TrimPrefix(line[len("data:"):], " ")
			additional := len(value)
			if data.Len() > 0 {
				additional++
			}
			if data.Len()+additional > t.limits.MaxEventBytes {
				report(errors.New("SSE event byte limit exceeded"))
				return
			}
			if data.Len() > 0 {
				data.WriteByte('\n')
			}
			data.WriteString(value)
		}
		if err != nil {
			report(err)
			return
		}
	}
}

func (t *networkSSE) Send(frame []byte) error {
	t.endpointMu.RLock()
	endpoint := t.endpoint
	t.endpointMu.RUnlock()
	if endpoint == "" {
		return errors.New("no SSE endpoint announced")
	}
	req, err := http.NewRequest("POST", endpoint, bytes.NewReader(frame))
	if err != nil {
		return err
	}
	req.Header.Set("Content-Type", "application/json")
	t.auth(req)
	resp, err := t.postClient.Do(req)
	if err != nil {
		return fmt.Errorf("SSE POST: %w", err)
	}
	drained, drainErr := io.CopyN(io.Discard, resp.Body, t.limits.MaxPostResponseBytes+1)
	closeErr := resp.Body.Close()
	if drained > t.limits.MaxPostResponseBytes {
		return errors.New("SSE POST response byte limit exceeded")
	}
	if drainErr != nil && !errors.Is(drainErr, io.EOF) {
		return drainErr
	}
	if closeErr != nil {
		return closeErr
	}
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("SSE POST returned HTTP %d", resp.StatusCode)
	}
	return nil
}

func (t *networkSSE) Recv(timeout time.Duration) ([]byte, error) {
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case frame, ok := <-t.frames:
		if !ok {
			select {
			case err := <-t.errCh:
				return nil, err
			default:
				return nil, errors.New("SSE stream closed")
			}
		}
		return frame, nil
	case <-timer.C:
		return nil, errors.New("SSE receive timeout")
	}
}

func (t *networkSSE) Close() error {
	t.closeOnce.Do(func() {
		close(t.closed)
		if t.body != nil {
			_ = t.body.Close()
		}
		t.streamClient.CloseIdleConnections()
		t.postClient.CloseIdleConnections()
		clear(t.bearer)
		t.bearer = nil
	})
	return nil
}

type busSSE struct {
	client          *BusAuthorizer
	handle, traceID uint64
	closeOnce       sync.Once
}

func (a *BusAuthorizer) OpenSSE(ctx context.Context, traceID uint64, request SSERequest) (SSEStream, error) {
	if a == nil || a.caller == nil {
		return nil, errors.New("egress: streaming service is not configured")
	}
	body, err := json.Marshal(request)
	if err != nil {
		return nil, err
	}
	timeout := time.Duration(request.TimeoutMS) * time.Millisecond
	reply, err := a.caller.Call(ctx, EventSSEOpen, StageSSEOpen, traceID, timeout+2*time.Second, body)
	if err != nil {
		return nil, fmt.Errorf("egress SSE open: %w", err)
	}
	var opened streamOpenReply
	var failed streamRecvReply
	if json.Unmarshal(reply, &opened) != nil || opened.Handle == 0 {
		_ = json.Unmarshal(reply, &failed)
		if failed.Error != "" {
			return nil, errors.New(failed.Error)
		}
		return nil, errors.New("egress SSE open returned an invalid handle")
	}
	return &busSSE{client: a, handle: opened.Handle, traceID: traceID}, nil
}

func (s *busSSE) call(stage, event uint32, command streamCommand, timeout time.Duration) (streamRecvReply, error) {
	body, _ := json.Marshal(command)
	reply, err := s.client.caller.Call(context.Background(), event, stage, s.traceID, timeout+2*time.Second, body)
	if err != nil {
		return streamRecvReply{}, err
	}
	var decoded streamRecvReply
	if json.Unmarshal(reply, &decoded) != nil {
		return decoded, errors.New("egress SSE returned an invalid response")
	}
	if decoded.Error != "" {
		return decoded, errors.New(decoded.Error)
	}
	return decoded, nil
}

func (s *busSSE) Send(frame []byte) error {
	_, err := s.call(StageSSESend, EventSSESend, streamCommand{Handle: s.handle, Frame: frame}, 30*time.Second)
	return err
}
func (s *busSSE) Recv(timeout time.Duration) ([]byte, error) {
	reply, err := s.call(StageSSERecv, EventSSERecv, streamCommand{Handle: s.handle, TimeoutMS: timeout.Milliseconds()}, timeout)
	return reply.Frame, err
}
func (s *busSSE) Close() error {
	var err error
	s.closeOnce.Do(func() { _, err = s.call(StageSSEClose, EventSSEClose, streamCommand{Handle: s.handle}, 5*time.Second) })
	return err
}
