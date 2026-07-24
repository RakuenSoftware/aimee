package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
)

// fakeMCPGraph stands up a fake aimee-server /v1/mcp/call that records the tool +
// arguments and returns the graph JSON wrapped as a text content node (mirroring the
// server's code_graph_passthrough -> text_content shape). `text` is the inner JSON
// (or an "error: ..." string the real tool emits on failure).
func fakeMCPGraph(t *testing.T, text string, gotTool *string, gotArgs *map[string]any) *config {
	mux := http.NewServeMux()
	mux.HandleFunc("/v1/mcp/call", func(w http.ResponseWriter, r *http.Request) {
		var body struct {
			Tool      string         `json:"tool"`
			Arguments map[string]any `json:"arguments"`
		}
		_ = json.NewDecoder(r.Body).Decode(&body)
		if gotTool != nil {
			*gotTool = body.Tool
		}
		if gotArgs != nil {
			*gotArgs = body.Arguments
		}
		inner, _ := json.Marshal(text)
		fmt.Fprintf(w, `{"status":"ok","content":[{"type":"text","text":%s}]}`, string(inner))
	})
	return startFakeV1(t, mux)
}

func TestHandleGraphHubsProxiesMCP(t *testing.T) {
	var tool string
	var args map[string]any
	s := &server{cfg: fakeMCPGraph(t, `{"status":"ok","hubs":[{"node":"hub","degree":3}]}`, &tool, &args)}

	rr := httptest.NewRecorder()
	s.handleGraphHubs(rr, httptest.NewRequest(http.MethodGet, "/api/graph/hubs?project=proj-alpha&max_results=10", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if tool != "index_graph_hubs" {
		t.Fatalf("tool=%q want index_graph_hubs", tool)
	}
	if args["project"] != "proj-alpha" {
		t.Fatalf("project arg=%v", args["project"])
	}
	if !strings.Contains(rr.Body.String(), `"node":"hub"`) {
		t.Fatalf("body=%q (expected verbatim hub JSON)", rr.Body.String())
	}
}

func TestHandleGraphNeighborsProxiesMCP(t *testing.T) {
	var tool string
	var args map[string]any
	s := &server{cfg: fakeMCPGraph(t, `{"status":"ok","node":"hub","neighbors":[]}`, &tool, &args)}

	rr := httptest.NewRecorder()
	s.handleGraphNeighbors(rr, httptest.NewRequest(http.MethodGet, "/api/graph/neighbors?project=p&node=hub", nil))

	if rr.Code != http.StatusOK {
		t.Fatalf("code=%d body=%q", rr.Code, rr.Body.String())
	}
	if tool != "index_graph_node" || args["node"] != "hub" {
		t.Fatalf("tool=%q node=%v", tool, args["node"])
	}
}

func TestHandleGraphNeighborsRequiresNode(t *testing.T) {
	s := &server{cfg: fakeMCPGraph(t, `{}`, nil, nil)}
	rr := httptest.NewRecorder()
	s.handleGraphNeighbors(rr, httptest.NewRequest(http.MethodGet, "/api/graph/neighbors?project=p", nil))
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("code=%d want 400", rr.Code)
	}
}

func TestHandleGraphHubsRequiresProject(t *testing.T) {
	s := &server{cfg: fakeMCPGraph(t, `{}`, nil, nil)}
	rr := httptest.NewRecorder()
	s.handleGraphHubs(rr, httptest.NewRequest(http.MethodGet, "/api/graph/hubs", nil))
	if rr.Code != http.StatusBadRequest {
		t.Fatalf("code=%d want 400", rr.Code)
	}
}

func TestHandleGraphSurprisingForwardsJudge(t *testing.T) {
	var tool string
	var args map[string]any
	s := &server{cfg: fakeMCPGraph(t, `{"status":"ok","links":[]}`, &tool, &args)}

	rr := httptest.NewRecorder()
	s.handleGraphSurprising(rr, httptest.NewRequest(http.MethodGet, "/api/graph/surprising?project=p&judge=true", nil))

	if rr.Code != http.StatusOK || tool != "index_graph_surprising" {
		t.Fatalf("code=%d tool=%q", rr.Code, tool)
	}
	if args["judge"] != true {
		t.Fatalf("judge arg=%v want true", args["judge"])
	}
}

// A tool that returns an "error: ..." text (e.g. KB down) maps to a 5xx JSON error,
// not a 200 with the error string masquerading as data.
func TestHandleGraphToolErrorMapsTo5xx(t *testing.T) {
	s := &server{cfg: fakeMCPGraph(t, `error: index_graph_hubs request failed (status 503)`, nil, nil)}
	rr := httptest.NewRecorder()
	s.handleGraphHubs(rr, httptest.NewRequest(http.MethodGet, "/api/graph/hubs?project=p", nil))
	if rr.Code != http.StatusBadGateway {
		t.Fatalf("code=%d want 502", rr.Code)
	}
	if !strings.Contains(rr.Body.String(), "request failed") {
		t.Fatalf("body=%q", rr.Body.String())
	}
}
