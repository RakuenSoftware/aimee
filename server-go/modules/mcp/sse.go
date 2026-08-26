package mcp

import (
	"bufio"
	"bytes"
	"context"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

type sseLimits struct {
	maxLineBytes         int
	maxEventBytes        int
	maxPostResponseBytes int64
	maxConnectionBytes   int64
	idleTimeout          time.Duration
	connectionLifetime   time.Duration
	postTimeout          time.Duration
}

var defaultSSELimits = sseLimits{
	maxLineBytes:         64 << 10,
	maxEventBytes:        1 << 20,
	maxPostResponseBytes: 64 << 10,
	maxConnectionBytes:   256 << 20,
	idleTimeout:          2 * time.Minute,
	connectionLifetime:   24 * time.Hour,
	postTimeout:          30 * time.Second,
}

// SSETransport speaks MCP over the HTTP+SSE transport.
//
// This is the last capability the C client had and this module did not.
// `aimee.yaml` MCP clients have always supported two transports, and a plugin
// module that could only spawn a local process could not host a remote server at
// all -- so the C path could not be retired without deleting that capability
// outright, whatever the migration story.
//
// The shape mirrors src/modules/protocols/mcp/mcp_client.c exactly, because the
// Go module is proven against the C client:
//
//	GET  url                   -> an SSE stream. The server's FIRST job is an
//	                              `endpoint` event naming where to POST.
//	POST <that endpoint>       -> a JSON-RPC frame; replies arrive back on the
//	                              SSE stream as `message` events.
//
// A bearer token, when set, rides every request as "Authorization: Bearer <t>".
type SSETransport struct {
	streamClient *http.Client
	postClient   *http.Client
	baseURL      string
	bearer       string
	limits       sseLimits

	body io.ReadCloser

	// endpoint is learned from the stream, so Send has to wait for it. A POST
	// before the `endpoint` event has nowhere to go.
	endpointOnce sync.Once
	endpointCh   chan string
	endpoint     string
	endpointMu   sync.RWMutex

	frames chan []byte
	errCh  chan error

	closeOnce sync.Once
	closed    chan struct{}
}

// NewSSETransport opens the event stream and waits for the endpoint event.
//
// The wait is the point: returning a transport whose Send has nowhere to POST
// would turn a server that never announced its endpoint into a confusing
// per-call timeout instead of one clear connect failure.
func NewSSETransport(rawURL, bearer string, timeout time.Duration) (*SSETransport, error) {
	return newSSETransportWithLimits(rawURL, bearer, timeout, defaultSSELimits)
}

func newSSETransportWithLimits(rawURL, bearer string, timeout time.Duration, limits sseLimits) (*SSETransport, error) {
	if rawURL == "" {
		return nil, fmt.Errorf("%w: empty SSE url", ErrTransport)
	}
	parsed, err := url.Parse(rawURL)
	if err != nil || parsed.Scheme == "" || parsed.Host == "" {
		return nil, fmt.Errorf("%w: bad SSE url: %v", ErrTransport, err)
	}
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	if limits.maxLineBytes <= 0 || limits.maxEventBytes <= 0 ||
		limits.maxPostResponseBytes <= 0 || limits.maxConnectionBytes <= 0 ||
		limits.idleTimeout <= 0 || limits.connectionLifetime <= 0 || limits.postTimeout <= 0 {
		return nil, fmt.Errorf("%w: invalid SSE resource limits", ErrTransport)
	}

	transport := &http.Transport{
		Proxy:                  http.ProxyFromEnvironment,
		ResponseHeaderTimeout:  timeout,
		MaxResponseHeaderBytes: 64 << 10,
	}
	postTransport := transport.Clone()

	t := &SSETransport{
		streamClient: &http.Client{Transport: transport},
		postClient:   &http.Client{Transport: postTransport, Timeout: limits.postTimeout},
		baseURL:      rawURL,
		bearer:       bearer,
		limits:       limits,
		endpointCh:   make(chan string, 1),
		frames:       make(chan []byte, 16),
		errCh:        make(chan error, 1),
		closed:       make(chan struct{}),
	}

	ctx, cancel := context.WithTimeout(context.Background(), limits.connectionLifetime)
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, rawURL, nil)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("%w: %v", ErrTransport, err)
	}
	req.Header.Set("Accept", "text/event-stream")
	t.auth(req)

	resp, err := t.streamClient.Do(req)
	if err != nil {
		cancel()
		return nil, fmt.Errorf("%w: SSE connect: %v", ErrTransport, err)
	}
	if resp.StatusCode/100 != 2 {
		cancel()
		resp.Body.Close()
		return nil, fmt.Errorf("%w: SSE connect: HTTP %d", ErrTransport, resp.StatusCode)
	}
	t.body = resp.Body
	go t.readLoop(cancel)

	select {
	case ep := <-t.endpointCh:
		t.endpointMu.Lock()
		t.endpoint = ep
		t.endpointMu.Unlock()
		return t, nil
	case err := <-t.errCh:
		t.Close()
		return nil, err
	case <-time.After(timeout):
		t.Close()
		return nil, fmt.Errorf("%w: no endpoint event within %s", ErrTimeout, timeout)
	}
}

func (t *SSETransport) auth(req *http.Request) {
	if t.bearer != "" {
		req.Header.Set("Authorization", "Bearer "+t.bearer)
	}
}

// resolveEndpoint turns the endpoint event's value into an absolute URL. Servers
// commonly send a path ("/messages?sessionId=..."), which is only meaningful
// relative to the stream's own URL.
func (t *SSETransport) resolveEndpoint(value string) string {
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

func readBoundedSSELine(reader *bufio.Reader, maxBytes int) ([]byte, error) {
	var line []byte
	for {
		part, err := reader.ReadSlice('\n')
		if len(line)+len(part) > maxBytes {
			return nil, fmt.Errorf("SSE line exceeds %d bytes", maxBytes)
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

// readLoop parses the SSE stream into endpoint announcements and JSON frames.
func (t *SSETransport) readLoop(cancel context.CancelFunc) {
	defer cancel()
	defer close(t.frames)

	reader := bufio.NewReaderSize(t.body, min(t.limits.maxLineBytes, 64<<10))
	var eventName string
	var data bytes.Buffer
	var connectionBytes int64
	idle := time.AfterFunc(t.limits.idleTimeout, func() { _ = t.body.Close() })
	defer idle.Stop()
	reportError := func(err error) {
		select {
		case t.errCh <- fmt.Errorf("%w: SSE stream: %v", ErrTransport, err):
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
				return fmt.Errorf("endpoint must retain the SSE origin")
			}
			t.endpointOnce.Do(func() {
				select {
				case t.endpointCh <- resolved:
				default:
				}
			})
			return nil
		}
		// Everything else is a JSON-RPC frame. An unnamed event is a message
		// too: the SSE default event type is "message", and servers rely on it.
		select {
		case t.frames <- []byte(payload):
		case <-t.closed:
		}
		return nil
	}

	for {
		lineBytes, err := readBoundedSSELine(reader, t.limits.maxLineBytes)
		connectionBytes += int64(len(lineBytes))
		if connectionBytes > t.limits.maxConnectionBytes {
			reportError(fmt.Errorf("connection exceeds %d bytes", t.limits.maxConnectionBytes))
			return
		}
		if len(lineBytes) > 0 {
			if !idle.Stop() {
				select {
				case <-idle.C:
				default:
				}
			}
			idle.Reset(t.limits.idleTimeout)
		}
		line := string(lineBytes)
		if len(line) > 0 {
			line = strings.TrimRight(line, "\r\n")
			switch {
			case line == "":
				if dispatchErr := dispatch(); dispatchErr != nil {
					reportError(dispatchErr)
					return
				}
			case strings.HasPrefix(line, ":"):
				// A comment / keep-alive. Ignored, but it is what stops an idle
				// stream from looking dead to an intermediary.
			case strings.HasPrefix(line, "event:"):
				eventName = strings.TrimSpace(line[len("event:"):])
			case strings.HasPrefix(line, "data:"):
				additional := len(line[len("data:"):])
				if data.Len() > 0 {
					additional++
				}
				if data.Len()+additional > t.limits.maxEventBytes {
					reportError(fmt.Errorf("event exceeds %d bytes", t.limits.maxEventBytes))
					return
				}
				if data.Len() > 0 {
					data.WriteByte('\n')
				}
				data.WriteString(strings.TrimPrefix(strings.TrimPrefix(line[len("data:"):], " "), ""))
			default:
				// id:/retry: and anything else are not part of this contract.
			}
		}
		if err != nil {
			reportError(err)
			return
		}
	}
}

// Send POSTs one JSON-RPC frame to the endpoint the stream announced.
func (t *SSETransport) Send(frame []byte) error {
	t.endpointMu.RLock()
	endpoint := t.endpoint
	t.endpointMu.RUnlock()
	if endpoint == "" {
		return fmt.Errorf("%w: no endpoint announced", ErrTransport)
	}

	req, err := http.NewRequest(http.MethodPost, endpoint, bytes.NewReader(frame))
	if err != nil {
		return fmt.Errorf("%w: %v", ErrTransport, err)
	}
	req.Header.Set("Content-Type", "application/json")
	t.auth(req)

	resp, err := t.postClient.Do(req)
	if err != nil {
		return fmt.Errorf("%w: POST: %v", ErrTransport, err)
	}
	// The reply arrives on the SSE stream, not here; the body is drained so the
	// connection can be reused rather than leaked per call.
	drained, drainErr := io.CopyN(io.Discard, resp.Body, t.limits.maxPostResponseBytes+1)
	closeErr := resp.Body.Close()
	if drained > t.limits.maxPostResponseBytes {
		return fmt.Errorf("%w: POST response exceeds %d bytes", ErrTransport, t.limits.maxPostResponseBytes)
	}
	if drainErr != nil && !errors.Is(drainErr, io.EOF) {
		return fmt.Errorf("%w: POST response: %v", ErrTransport, drainErr)
	}
	if closeErr != nil {
		return fmt.Errorf("%w: POST response close: %v", ErrTransport, closeErr)
	}
	if resp.StatusCode/100 != 2 {
		return fmt.Errorf("%w: POST returned HTTP %d", ErrTransport, resp.StatusCode)
	}
	return nil
}

// Recv returns the next frame from the stream, or ErrTimeout.
func (t *SSETransport) Recv(timeout time.Duration) ([]byte, error) {
	if timeout <= 0 {
		return nil, ErrTimeout
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case frame, ok := <-t.frames:
		if !ok {
			select {
			case err := <-t.errCh:
				return nil, err
			default:
				return nil, fmt.Errorf("%w: SSE stream closed", ErrTransport)
			}
		}
		return frame, nil
	case <-timer.C:
		return nil, ErrTimeout
	}
}

// Close ends the stream. Safe to call more than once.
func (t *SSETransport) Close() error {
	t.closeOnce.Do(func() {
		close(t.closed)
		if t.body != nil {
			t.body.Close()
		}
		t.streamClient.CloseIdleConnections()
		t.postClient.CloseIdleConnections()
	})
	return nil
}
