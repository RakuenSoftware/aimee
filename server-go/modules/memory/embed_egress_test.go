package memory

import (
	"context"
	"fmt"
	"net/http"
	"net/http/httptest"
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
	vector := Embed(context.Background(), 2, executor, EmbedRequest{
		BaseURL: server.URL, Text: "probe", InputType: "document", MaxDim: 3})
	if vector.Error != "" || vector.Dim != 3 {
		t.Fatalf("embedding through governed egress: %+v", vector)
	}
}
