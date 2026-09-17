package memory

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Exercise the actual memory -> egress wire contract and policy. The ordinary
// embed transport stub intentionally does not enforce caller permissions.
type governedEmbedCaller struct{ handler bus.ModuleHandler }

func (c governedEmbedCaller) Call(_ context.Context, _ uint32, stage uint32,
	trace uint64, _ time.Duration, body []byte) ([]byte, error) {
	reply, status := c.handler(bus.ModuleInvocation{StageID: stage,
		PrincipalClass: 1, PrincipalRef: egress.MemoryClientRef, TraceID: trace}, body)
	if status != bus.ModuleStatusOK {
		return nil, fmt.Errorf("egress status %d", status)
	}
	return reply, nil
}

func TestEmbedServingIdentityThroughGovernedLocalEgress(t *testing.T) {
	resetBreaker(t)
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		switch {
		case r.Method == "GET" && r.URL.Path == "/health":
			_, _ = w.Write([]byte(`{"status":"ok","model":"test-model","serving_id":"test-space-v1"}`))
		case r.Method == "POST" && r.URL.Path == "/embed_batch":
			_, _ = w.Write([]byte(`[[0.25,0.5,0.75],[1,2,3]]`))
		case r.Method == "POST" && r.URL.Path == "/embed":
			_, _ = w.Write([]byte(`[0.25,0.5,0.75]`))
		default:
			http.NotFound(w, r)
		}
	}))
	defer server.Close()
	executor, err := egress.NewBusAuthorizer(governedEmbedCaller{egress.NewHandler()})
	if err != nil {
		t.Fatal(err)
	}
	identity := EmbedServingID(context.Background(), 1, executor, server.URL)
	if identity.Error != "" || identity.ServingID != "test-space-v1" {
		t.Fatalf("identity through governed egress: %+v", identity)
	}
	batch := EmbedBatch(t.Context(), 3, executor, EmbedRequest{BaseURL: server.URL, Texts: []string{"a", "b"}, InputType: "document", MaxDim: 3})
	if batch.Error != "" || len(batch.Vectors) != 2 {
		t.Fatal(batch)
	}
	vector := Embed(context.Background(), 2, executor, EmbedRequest{
		BaseURL: server.URL, Text: "probe", InputType: "document", MaxDim: 3})
	if vector.Error != "" || vector.Dim != 3 {
		t.Fatalf("embedding through governed egress: %+v", vector)
	}
}

func TestEmbedDimensionAndTruncation(t *testing.T) {
	resetBreaker(t)
	for _, command := range []string{"printf 'invalid'", "printf '0'", "printf '99999'"} {
		if got := EmbedDimension(t.Context(), 0, nil, EmbedRequest{BaseURL: command, MaxDim: 3}); got.Error == "" {
			t.Fatal(got)
		}
	}
	if got := EmbedDimension(t.Context(), 0, nil, EmbedRequest{BaseURL: "printf '3\\n'", MaxDim: 3}); got.Error != "" || got.Dim != 3 {
		t.Fatal(got)
	}
	handler := NewHandler(&batchExecutor{reply: `[1,2,3]`})
	frame, _ := bus.EncodeCommand("embed", []byte(`{"base_url":"http://embedder","text":"probe","max_dim":2}`))
	encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
	body, err := bus.DecodeCommandResult(encoded)
	if status != bus.ModuleStatusOK || err != nil || strings.Contains(string(body), `"vector"`) || !strings.Contains(string(body), `"truncated":true`) {
		t.Fatalf("%s %v", body, err)
	}
}
