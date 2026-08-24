package mcp

import (
	"bufio"
	"bytes"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

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
	client  *http.Client
	baseURL string
	bearer  string

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
	if rawURL == "" {
		return nil, fmt.Errorf("%w: empty SSE url", ErrTransport)
	}
	if _, err := url.Parse(rawURL); err != nil {
		return nil, fmt.Errorf("%w: bad SSE url: %v", ErrTransport, err)
	}
	if timeout <= 0 {
		timeout = 30 * time.Second
	}

	t := &SSETransport{
		// No client timeout: the GET is a long-lived stream, and a Client
		// timeout would cut it at the deadline rather than bounding a request.
		client:     &http.Client{},
		baseURL:    rawURL,
		bearer:     bearer,
		endpointCh: make(chan string, 1),
		frames:     make(chan []byte, 16),
		errCh:      make(chan error, 1),
		closed:     make(chan struct{}),
	}

	req, err := http.NewRequest(http.MethodGet, rawURL, nil)
	if err != nil {
		return nil, fmt.Errorf("%w: %v", ErrTransport, err)
	}
	req.Header.Set("Accept", "text/event-stream")
	t.auth(req)

	resp, err := t.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("%w: SSE connect: %v", ErrTransport, err)
	}
	if resp.StatusCode/100 != 2 {
		resp.Body.Close()
		return nil, fmt.Errorf("%w: SSE connect: HTTP %d", ErrTransport, resp.StatusCode)
	}
	t.body = resp.Body
	go t.readLoop()

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
		return value
	}
	ref, err := url.Parse(value)
	if err != nil {
		return value
	}
	return base.ResolveReference(ref).String()
}

// readLoop parses the SSE stream into endpoint announcements and JSON frames.
func (t *SSETransport) readLoop() {
	defer close(t.frames)

	reader := bufio.NewReaderSize(t.body, 1<<16)
	var eventName string
	var data bytes.Buffer

	dispatch := func() {
		if data.Len() == 0 {
			eventName = ""
			return
		}
		payload := strings.TrimRight(data.String(), "\n")
		data.Reset()
		name := eventName
		eventName = ""

		if name == "endpoint" {
			t.endpointOnce.Do(func() {
				select {
				case t.endpointCh <- t.resolveEndpoint(strings.TrimSpace(payload)):
				default:
				}
			})
			return
		}
		// Everything else is a JSON-RPC frame. An unnamed event is a message
		// too: the SSE default event type is "message", and servers rely on it.
		select {
		case t.frames <- []byte(payload):
		case <-t.closed:
		}
	}

	for {
		line, err := reader.ReadString('\n')
		if len(line) > 0 {
			line = strings.TrimRight(line, "\r\n")
			switch {
			case line == "":
				dispatch() // a blank line terminates one event
			case strings.HasPrefix(line, ":"):
				// A comment / keep-alive. Ignored, but it is what stops an idle
				// stream from looking dead to an intermediary.
			case strings.HasPrefix(line, "event:"):
				eventName = strings.TrimSpace(line[len("event:"):])
			case strings.HasPrefix(line, "data:"):
				if data.Len() > 0 {
					data.WriteByte('\n')
				}
				data.WriteString(strings.TrimPrefix(strings.TrimPrefix(line[len("data:"):], " "), ""))
			default:
				// id:/retry: and anything else are not part of this contract.
			}
		}
		if err != nil {
			select {
			case t.errCh <- fmt.Errorf("%w: SSE stream: %v", ErrTransport, err):
			default:
			}
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

	resp, err := t.client.Do(req)
	if err != nil {
		return fmt.Errorf("%w: POST: %v", ErrTransport, err)
	}
	// The reply arrives on the SSE stream, not here; the body is drained so the
	// connection can be reused rather than leaked per call.
	_, _ = io.Copy(io.Discard, resp.Body)
	resp.Body.Close()
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
		t.client.CloseIdleConnections()
	})
	return nil
}
