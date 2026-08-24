package mcp

import (
	"bufio"
	"bytes"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"os/exec"
	"sync"
	"time"
)

// Errors this package returns. Callers distinguish "the plugin said no" from
// "the plugin is not reachable": the first is a tool result, the second takes
// the module out of service until the plugin comes back.
var (
	ErrTransport = errors.New("mcp: transport failed")
	ErrProtocol  = errors.New("mcp: protocol violation")
	ErrTimeout   = errors.New("mcp: timed out")
)

// Transport carries newline-delimited JSON-RPC frames to one plugin.
//
// This mirrors the C transport vtable in
// src/modules/protocols/include/aimee/protocols/mcp/mcp_client.h (send/recv/close)
// on purpose: the Go module is proven against the C client, and a matching seam
// is what lets a conformance test drive both with the same script.
type Transport interface {
	// Send writes one frame. The frame must not contain an embedded newline.
	Send(frame []byte) error
	// Recv blocks up to timeout for one complete frame. A timeout returns
	// ErrTimeout rather than an empty frame, so a caller cannot mistake
	// "nothing yet" for "an empty answer".
	Recv(timeout time.Duration) ([]byte, error)
	// Close releases the transport. Safe on a partially initialised transport
	// and safe to call more than once.
	Close() error
}

// StdioTransport runs the plugin as a child process and speaks over its stdin
// and stdout. The child's stderr is inherited so a crash surfaces in this
// process's logs rather than being swallowed.
type StdioTransport struct {
	cmd    *exec.Cmd
	stdin  io.WriteCloser
	stdout *bufio.Reader

	mu     sync.Mutex
	closed bool

	// frames carries lines off a reader goroutine so Recv can honour a timeout.
	// A blocking Read on a child's pipe is not otherwise interruptible, and a
	// module that cannot time out a wedged plugin is a module that wedges.
	frames chan []byte
	readEr chan error
}

// NewStdioTransport starts argv in dir (dir may be empty for the current
// directory) and returns a transport over its stdio.
func NewStdioTransport(argv []string, dir string, stderr io.Writer) (*StdioTransport, error) {
	if len(argv) == 0 {
		return nil, fmt.Errorf("%w: empty argv", ErrTransport)
	}
	cmd := exec.Command(argv[0], argv[1:]...)
	cmd.Dir = dir
	cmd.Stderr = stderr

	stdin, err := cmd.StdinPipe()
	if err != nil {
		return nil, fmt.Errorf("%w: stdin: %v", ErrTransport, err)
	}
	stdout, err := cmd.StdoutPipe()
	if err != nil {
		stdin.Close()
		return nil, fmt.Errorf("%w: stdout: %v", ErrTransport, err)
	}
	if err := cmd.Start(); err != nil {
		stdin.Close()
		return nil, fmt.Errorf("%w: start %s: %v", ErrTransport, argv[0], err)
	}

	t := &StdioTransport{
		cmd:    cmd,
		stdin:  stdin,
		stdout: bufio.NewReaderSize(stdout, 1<<16),
		frames: make(chan []byte, 8),
		readEr: make(chan error, 1),
	}
	go t.readLoop()
	return t, nil
}

func (t *StdioTransport) readLoop() {
	for {
		line, err := t.stdout.ReadBytes('\n')
		if len(line) > 0 {
			t.frames <- bytes.TrimRight(line, "\r\n")
		}
		if err != nil {
			t.readEr <- err
			close(t.frames)
			return
		}
	}
}

func (t *StdioTransport) Send(frame []byte) error {
	t.mu.Lock()
	defer t.mu.Unlock()
	if t.closed {
		return fmt.Errorf("%w: send on closed transport", ErrTransport)
	}
	if bytes.ContainsAny(frame, "\r\n") {
		return fmt.Errorf("%w: frame contains a newline", ErrProtocol)
	}
	if _, err := t.stdin.Write(append(frame, '\n')); err != nil {
		return fmt.Errorf("%w: write: %v", ErrTransport, err)
	}
	return nil
}

func (t *StdioTransport) Recv(timeout time.Duration) ([]byte, error) {
	if timeout <= 0 {
		return nil, ErrTimeout
	}
	timer := time.NewTimer(timeout)
	defer timer.Stop()
	select {
	case frame, ok := <-t.frames:
		if !ok {
			select {
			case err := <-t.readEr:
				return nil, fmt.Errorf("%w: read: %v", ErrTransport, err)
			default:
				return nil, fmt.Errorf("%w: transport closed", ErrTransport)
			}
		}
		return frame, nil
	case <-timer.C:
		return nil, ErrTimeout
	}
}

func (t *StdioTransport) Close() error {
	t.mu.Lock()
	if t.closed {
		t.mu.Unlock()
		return nil
	}
	t.closed = true
	t.mu.Unlock()

	t.stdin.Close()
	if t.cmd.Process != nil {
		_ = t.cmd.Process.Kill()
		_ = t.cmd.Wait()
	}
	return nil
}

// --- JSON-RPC 2.0 framing -------------------------------------------------

type rpcRequest struct {
	JSONRPC string `json:"jsonrpc"`
	ID      uint64 `json:"id"`
	Method  string `json:"method"`
	Params  any    `json:"params,omitempty"`
}

type rpcError struct {
	Code    int    `json:"code"`
	Message string `json:"message"`
}

type rpcResponse struct {
	JSONRPC string          `json:"jsonrpc"`
	ID      uint64          `json:"id"`
	Result  json.RawMessage `json:"result,omitempty"`
	Error   *rpcError       `json:"error,omitempty"`
}

// Tool is one callable the plugin advertises. The field names match MCP's
// tools/list shape so the JSON decodes directly.
type Tool struct {
	Name        string          `json:"name"`
	Description string          `json:"description"`
	InputSchema json.RawMessage `json:"inputSchema"`
	// Annotations carries MCP's optional behaviour hints (readOnlyHint,
	// destructiveHint). It is the only machine-readable signal about what a
	// tool actually does, so it is what the permission ceiling is checked
	// against.
	Annotations json.RawMessage `json:"annotations"`
}

// Client is a session with exactly ONE plugin. One plugin per module is the
// governing rule, so this deliberately holds a single transport and has no
// notion of a registry: the module instance IS the scope.
type Client struct {
	transport Transport
	timeout   time.Duration

	// callMu serialises whole request/response round trips.
	//
	// The bus runtime allows up to 16 in-flight invocations per module, but one
	// plugin is one pair of pipes. Two concurrent round trips would interleave
	// on the same reader, and since each skips frames whose id is not its own,
	// they would consume each other's responses and both block until the
	// deadline. Serialising is correct rather than merely safe here: a stdio
	// MCP server has no request concurrency to exploit.
	callMu sync.Mutex

	mu     sync.Mutex
	nextID uint64
	tools  []Tool
	ready  bool
}

// NewClient wraps a transport. timeout bounds every individual request.
func NewClient(transport Transport, timeout time.Duration) *Client {
	if timeout <= 0 {
		timeout = 30 * time.Second
	}
	return &Client{transport: transport, timeout: timeout, nextID: 1}
}

// call performs one request/response round trip.
//
// Frames whose id does not match the outstanding request are skipped rather
// than treated as the answer: MCP servers may interleave notifications (which
// carry no id) with responses, and reading one as a result would return the
// wrong body under load rather than failing.
func (c *Client) call(method string, params any) (json.RawMessage, error) {
	c.callMu.Lock()
	defer c.callMu.Unlock()

	c.mu.Lock()
	id := c.nextID
	c.nextID++
	c.mu.Unlock()

	frame, err := json.Marshal(rpcRequest{JSONRPC: "2.0", ID: id, Method: method, Params: params})
	if err != nil {
		return nil, fmt.Errorf("%w: encode %s: %v", ErrProtocol, method, err)
	}
	if err := c.transport.Send(frame); err != nil {
		return nil, err
	}

	deadline := time.Now().Add(c.timeout)
	for {
		remaining := time.Until(deadline)
		if remaining <= 0 {
			return nil, fmt.Errorf("%w: %s", ErrTimeout, method)
		}
		raw, err := c.transport.Recv(remaining)
		if err != nil {
			return nil, err
		}
		var resp rpcResponse
		if err := json.Unmarshal(raw, &resp); err != nil {
			// A frame we cannot parse is not necessarily ours; keep waiting
			// until the deadline rather than failing the call on a stray line.
			continue
		}
		if resp.ID != id {
			continue
		}
		if resp.Error != nil {
			return nil, fmt.Errorf("%w: %s: %s (%d)", ErrProtocol, method, resp.Error.Message,
				resp.Error.Code)
		}
		return resp.Result, nil
	}
}

// Initialize performs the MCP handshake. It must succeed before ListTools.
func (c *Client) Initialize(clientName, clientVersion string) error {
	params := map[string]any{
		"protocolVersion": "2024-11-05",
		"capabilities":    map[string]any{},
		"clientInfo":      map[string]any{"name": clientName, "version": clientVersion},
	}
	if _, err := c.call("initialize", params); err != nil {
		return err
	}
	c.mu.Lock()
	c.ready = true
	c.mu.Unlock()
	return nil
}

// ListTools fetches and caches the plugin's tools. Calling it again refreshes
// the cache, which is how a reconnect picks up a changed tool set.
func (c *Client) ListTools() ([]Tool, error) {
	raw, err := c.call("tools/list", map[string]any{})
	if err != nil {
		return nil, err
	}
	var payload struct {
		Tools []Tool `json:"tools"`
	}
	if err := json.Unmarshal(raw, &payload); err != nil {
		return nil, fmt.Errorf("%w: tools/list: %v", ErrProtocol, err)
	}
	c.mu.Lock()
	c.tools = payload.Tools
	c.mu.Unlock()
	return payload.Tools, nil
}

// Tools returns the cached tool set without going to the plugin.
func (c *Client) Tools() []Tool {
	c.mu.Lock()
	defer c.mu.Unlock()
	out := make([]Tool, len(c.tools))
	copy(out, c.tools)
	return out
}

// Ready reports whether the handshake completed.
func (c *Client) Ready() bool {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.ready
}

// CallTool invokes one tool and returns the raw MCP result object.
func (c *Client) CallTool(name string, args json.RawMessage) (json.RawMessage, error) {
	params := map[string]any{"name": name}
	if len(args) > 0 {
		params["arguments"] = args
	}
	return c.call("tools/call", params)
}

// Close shuts the session down.
func (c *Client) Close() error {
	c.mu.Lock()
	c.ready = false
	c.mu.Unlock()
	return c.transport.Close()
}
