package egress

import (
	"context"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/http/httptest"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func TestNetworkSSEOwnsStreamPostAndBearer(t *testing.T) {
	var mu sync.Mutex
	var bearer []string
	frames := make(chan string, 2)
	mux := http.NewServeMux()
	mux.HandleFunc("/sse", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		bearer = append(bearer, r.Header.Get("Authorization"))
		mu.Unlock()
		w.Header().Set("Content-Type", "text/event-stream")
		fmt.Fprint(w, "event: endpoint\ndata: /messages\n\n")
		w.(http.Flusher).Flush()
		select {
		case frame := <-frames:
			fmt.Fprintf(w, "event: message\ndata: %s\n\n", frame)
			w.(http.Flusher).Flush()
		case <-r.Context().Done():
		}
	})
	mux.HandleFunc("/messages", func(w http.ResponseWriter, r *http.Request) {
		mu.Lock()
		bearer = append(bearer, r.Header.Get("Authorization"))
		mu.Unlock()
		body, _ := io.ReadAll(r.Body)
		frames <- string(body)
		w.WriteHeader(http.StatusAccepted)
	})
	server := httptest.NewServer(mux)
	defer server.Close()
	policy := policy{resolver: fixedResolver{{IP: net.ParseIP("127.0.0.1")}}, allowPrivateMCP: true}
	service := newStreamService(policy, staticCredentialResolver([]byte("secret")))
	request := SSERequest{URL: server.URL + "/sse", CredentialHandle: "mcp:712", TimeoutMS: 1000, Limits: DefaultSSELimits()}
	stream, err := service.open(bus.ModuleInvocation{PrincipalClass: 1,
		PrincipalRef: 200 + PluginClientOffset, StageID: StageSSEOpen}, request)
	if err != nil {
		t.Fatal(err)
	}
	defer stream.Close()
	frame := []byte(`{"jsonrpc":"2.0","id":1}`)
	if err := stream.Send(frame); err != nil {
		t.Fatal(err)
	}
	received, err := stream.Recv(time.Second)
	if err != nil || string(received) != string(frame) {
		t.Fatalf("received=%s err=%v", received, err)
	}
	mu.Lock()
	defer mu.Unlock()
	if len(bearer) != 2 || bearer[0] != "Bearer secret" || bearer[1] != "Bearer secret" {
		encoded, _ := json.Marshal(bearer)
		t.Fatalf("bearers=%s", encoded)
	}
}

type staticCredentialResolver []byte

func (r staticCredentialResolver) Resolve(_ context.Context, _ uint32, _ string) ([]byte, error) {
	return append([]byte(nil), r...), nil
}
