package memory

import (
	"context"
	"encoding/json"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

type batchExecutor struct {
	reply          string
	calls          int
	timeout        int64
	body, endpoint string
	status         int
}

func (e *batchExecutor) Authorize(context.Context, uint64, egress.Request) (egress.Decision, error) {
	return egress.Decision{Allowed: true}, nil
}
func (e *batchExecutor) Do(ctx context.Context, _ uint64, request egress.HTTPRequest) (egress.HTTPResponse, error) {
	e.calls++
	e.timeout = request.TimeoutMS
	e.body = string(request.Body)
	e.endpoint = request.TargetURL
	status := e.status
	if status == 0 {
		status = 200
	}
	return egress.HTTPResponse{Status: status, Body: []byte(e.reply)}, nil
}
func TestEmbedBatchParity(t *testing.T) {
	resetBreaker(t)
	t.Setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", "77000")
	executor := &batchExecutor{reply: `[[0,0.5,0.25],[1,1.5,1.25],[2,2.5,2.25],[3,3.5,3.25]]`}
	request := EmbedRequest{Operation: "batch", BaseURL: "http://embedder", InputType: "document", Texts: []string{"alpha", "beta", "gamma", "delta"}, MaxDim: 3}
	response := EmbedBatch(t.Context(), 1, executor, request)
	if response.Error != "" || len(response.Vectors) != 4 || executor.calls != 1 || executor.body != `["alpha","beta","gamma","delta"]` || executor.endpoint != "http://embedder/embed_batch?input_type=document" || executor.timeout != 77000 {
		t.Fatalf("%+v %+v", response, executor)
	}
	for i, row := range response.Vectors {
		if row[0] != float32(i) || row[1] != float32(i)+0.5 || row[2] != float32(i)+0.25 {
			t.Fatal(row)
		}
	}
	request.InputType = "query"
	if response = EmbedBatch(t.Context(), 1, executor, request); response.Error != "" || executor.endpoint != "http://embedder/embed_batch?input_type=query" {
		t.Fatal(response)
	}
	for _, reply := range []string{`[[1,2,3]]`, `[[1,2],[1,2],[1,2],[1,2]]`, `[[1,2,3],[1,2,3],[1,2,3],[1,2,1e39]]`, `[[1,2,null],[1,2,3],[1,2,3],[1,2,3]]`, `[null,null,null,null]`, `null`} {
		resetBreaker(t)
		executor.reply = reply
		response = EmbedBatch(t.Context(), 1, executor, request)
		if response.Error == "" || len(response.Vectors) != 0 || response.Dim != 0 {
			t.Fatalf("partial batch %s: %+v", reply, response)
		}
	}
	resetBreaker(t)
	executor.status = 403
	response = EmbedBatch(t.Context(), 1, executor, request)
	if !response.Unauthorized || len(response.Vectors) != 0 {
		t.Fatal(response)
	}
	executor.status = 500
	response = EmbedBatch(t.Context(), 1, executor, request)
	if response.Error == "" || len(response.Vectors) != 0 {
		t.Fatal(response)
	}
	for _, invalid := range []EmbedRequest{{BaseURL: "builtin", Texts: request.Texts, MaxDim: 3}, {BaseURL: request.BaseURL, MaxDim: 3}, {BaseURL: request.BaseURL, Texts: []string{"a", ""}, MaxDim: 3}} {
		calls := executor.calls
		if response = EmbedBatch(t.Context(), 1, executor, invalid); response.Error == "" || executor.calls != calls {
			t.Fatal(response)
		}
	}
	for _, value := range []string{"", "0", "banana", "120001"} {
		t.Setenv("AIMEE_EMBED_HTTP_TIMEOUT_MS", value)
		if embedHTTPTimeout() < time.Minute {
			t.Fatal(value)
		}
	}
}
func TestEmbedCommandIsHostOnly(t *testing.T) {
	resetBreaker(t)
	for _, placement := range []Placement{PlacementServer, PlacementKB} {
		executor := &batchExecutor{reply: `[1,2,3]`}
		handler := NewHandler(executor, WithDataStore(placement, nil))
		frame, _ := bus.EncodeCommand("embed", json.RawMessage(`{"base_url":"http://embedder","text":"probe","max_dim":3}`))
		if _, status := handler(bus.ModuleInvocation{StageID: StageCommand, PrincipalRef: 99}, frame); status != bus.ModuleStatusInvalidRequest || executor.calls != 0 {
			t.Fatal("untrusted embed accepted")
		}
		encoded, status := handler(bus.ModuleInvocation{StageID: StageCommand}, frame)
		body, err := bus.DecodeCommandResult(encoded)
		var response EmbedResponse
		if status != bus.ModuleStatusOK || err != nil || json.Unmarshal(body, &response) != nil || response.Dim != 3 {
			t.Fatalf("%s %d %v", encoded, status, err)
		}
	}
}
func TestEmbedRejectsFloatOverflow(t *testing.T) {
	resetBreaker(t)
	response := Embed(t.Context(), 1, &batchExecutor{reply: `[1,1e39]`}, EmbedRequest{BaseURL: "http://embedder", Text: "a", MaxDim: 2})
	if response.Error == "" || len(response.Vector) != 0 {
		t.Fatal(response)
	}
}
