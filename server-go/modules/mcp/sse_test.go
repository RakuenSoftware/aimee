package mcp

import (
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"net/http/httptest"
	"strings"
	"sync"
	"testing"
	"time"
)

// A minimal MCP-over-SSE server: GET announces the POST endpoint, POSTs are
// answered as `message` events back on the stream.
type sseServer struct {
	mu         sync.Mutex
	out        chan string
	bearers    []string
	noEndpoint bool
	relative   bool
}

func newSSEServer() *sseServer {
	return &sseServer{out: make(chan string, 16)}
}

func (s *sseServer) handler(t *testing.T) http.Handler {
	t.Helper()
	mux := http.NewServeMux()

	mux.HandleFunc("/sse", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.bearers = append(s.bearers, r.Header.Get("Authorization"))
		s.mu.Unlock()

		w.Header().Set("Content-Type", "text/event-stream")
		w.WriteHeader(http.StatusOK)
		flusher, _ := w.(http.Flusher)

		if !s.noEndpoint {
			target := "/messages"
			if !s.relative {
				target = "http://" + r.Host + "/messages"
			}
			fmt.Fprintf(w, "event: endpoint\ndata: %s\n\n", target)
			flusher.Flush()
		}
		// A keep-alive comment: it must be ignored, not parsed as a frame.
		fmt.Fprint(w, ": keep-alive\n\n")
		flusher.Flush()

		for {
			select {
			case msg := <-s.out:
				fmt.Fprintf(w, "event: message\ndata: %s\n\n", msg)
				flusher.Flush()
			case <-r.Context().Done():
				return
			}
		}
	})

	mux.HandleFunc("/messages", func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.bearers = append(s.bearers, r.Header.Get("Authorization"))
		s.mu.Unlock()

		body, _ := io.ReadAll(r.Body)
		var req rpcRequest
		if err := json.Unmarshal(body, &req); err != nil {
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		var result string
		switch req.Method {
		case "initialize":
			result = `{"protocolVersion":"2024-11-05"}`
		case "tools/list":
			result = `{"tools":[{"name":"remote-search","description":"Search remotely"}]}`
		case "tools/call":
			result = `{"content":[{"type":"text","text":"remote ok"}]}`
		default:
			result = ""
		}
		// The reply rides the STREAM, not this response. A client that read the
		// POST body as the answer would work against a lenient server and fail
		// against a spec-compliant one.
		w.WriteHeader(http.StatusAccepted)
		if result == "" {
			s.out <- fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"error":{"code":-32601,"message":"no"}}`, req.ID)
			return
		}
		s.out <- fmt.Sprintf(`{"jsonrpc":"2.0","id":%d,"result":%s}`, req.ID, result)
	})
	return mux
}

func (s *sseServer) seenBearer(want string) bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	for _, b := range s.bearers {
		if b != want {
			return false
		}
	}
	return len(s.bearers) > 0
}

func TestSSETransportCompletesAFullSession(t *testing.T) {
	srv := newSSEServer()
	ts := httptest.NewServer(srv.handler(t))
	defer ts.Close()

	tr, err := NewSSETransport(ts.URL+"/sse", "", 5*time.Second)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer tr.Close()

	client := NewClient(tr, 5*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatalf("initialize: %v", err)
	}
	tools, err := client.ListTools()
	if err != nil {
		t.Fatalf("tools/list: %v", err)
	}
	if len(tools) != 1 || tools[0].Name != "remote-search" {
		t.Fatalf("tools = %+v", tools)
	}
	res, err := client.CallTool("remote-search", nil)
	if err != nil {
		t.Fatalf("tools/call: %v", err)
	}
	if !strings.Contains(string(res), "remote ok") {
		t.Fatalf("result = %s", res)
	}
}

func TestSSETransportSendsTheBearerOnEveryRequest(t *testing.T) {
	// The stream GET and every POST must carry it; a token on only one of them
	// authenticates the session and then fails the calls.
	srv := newSSEServer()
	ts := httptest.NewServer(srv.handler(t))
	defer ts.Close()

	tr, err := NewSSETransport(ts.URL+"/sse", "s3cret", 5*time.Second)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer tr.Close()

	client := NewClient(tr, 5*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatalf("initialize: %v", err)
	}
	if !srv.seenBearer("Bearer s3cret") {
		t.Fatal("a request reached the server without the bearer token")
	}
}

func TestSSETransportResolvesARelativeEndpoint(t *testing.T) {
	// Servers commonly announce a PATH. Treating it as an absolute URL makes
	// every POST fail with an unhelpful transport error.
	srv := newSSEServer()
	srv.relative = true
	ts := httptest.NewServer(srv.handler(t))
	defer ts.Close()

	tr, err := NewSSETransport(ts.URL+"/sse", "", 5*time.Second)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	defer tr.Close()

	client := NewClient(tr, 5*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatalf("initialize with a relative endpoint: %v", err)
	}
}

func TestSSETransportRefusesAServerThatNeverAnnouncesAnEndpoint(t *testing.T) {
	// One clear connect failure beats a transport whose every Send has nowhere
	// to go and surfaces as a per-call timeout.
	srv := newSSEServer()
	srv.noEndpoint = true
	ts := httptest.NewServer(srv.handler(t))
	defer ts.Close()

	if _, err := NewSSETransport(ts.URL+"/sse", "", 300*time.Millisecond); err == nil {
		t.Fatal("a server that announced no endpoint was accepted")
	}
}

func TestSSETransportRefusesANonOKStream(t *testing.T) {
	ts := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.WriteHeader(http.StatusUnauthorized)
	}))
	defer ts.Close()
	if _, err := NewSSETransport(ts.URL+"/sse", "", time.Second); err == nil {
		t.Fatal("an HTTP 401 stream was accepted")
	}
}

func TestSSETransportIsUsableByTheModuleLikeAnyOther(t *testing.T) {
	// The Transport interface is the whole point: the module must not know or
	// care which transport its plugin is behind.
	srv := newSSEServer()
	ts := httptest.NewServer(srv.handler(t))
	defer ts.Close()

	tr, err := NewSSETransport(ts.URL+"/sse", "", 5*time.Second)
	if err != nil {
		t.Fatalf("connect: %v", err)
	}
	var iface Transport = tr
	client := NewClient(iface, 5*time.Second)
	if err := client.Initialize("aimee", "test"); err != nil {
		t.Fatal(err)
	}
	if _, err := client.ListTools(); err != nil {
		t.Fatal(err)
	}

	m := New("remote", PermDangerous)
	m.Attach(client)
	defer m.Detach()

	payload, status := m.Handle(invocation(StageDeclareCommands), EncodeDeclareRequest())
	if status != 0 {
		t.Fatalf("declare status = %v", status)
	}
	cmds := decodeCommands(t, payload)
	if len(cmds) != 1 || cmds[0].Group != "remote" || cmds[0].Verb != "remote_search" {
		t.Fatalf("commands = %+v", cmds)
	}
}
