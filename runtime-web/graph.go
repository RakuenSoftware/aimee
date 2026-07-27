package main

import (
	"encoding/json"
	"fmt"
	"net/http"
	"strconv"
	"strings"
)

// graph.go — §8 read-only code-graph visualization. The /v1/code/graph* routes live
// on aimee-kb, reachable from webchat only via aimee-server's MCP tools
// (index_graph_hubs / index_graph_surprising / index_graph_node), which forward the
// KB route's JSON verbatim as a text content node. These handlers expose that JSON to
// the browser at /api/graph/*; the SPA Graph page renders it.

// mcpText calls an MCP tool and returns its text content — the verbatim JSON the
// code_graph_* tools forward (or an "error: ..." string the tool emits on failure).
func (s *server) mcpText(r *http.Request, tool string, args map[string]any) (string, error) {
	if args == nil {
		args = map[string]any{}
	}
	resp, err := s.socketCallForRequest(r, map[string]any{
		"method":    "mcp.call",
		"tool":      tool,
		"arguments": args,
	})
	if err != nil {
		return "", err
	}
	raw, ok := resp["content"]
	if !ok {
		return "", fmt.Errorf("server response missing content")
	}
	var content []struct {
		Type string `json:"type"`
		Text string `json:"text"`
	}
	if err := json.Unmarshal(raw, &content); err != nil {
		return "", fmt.Errorf("invalid mcp content: %w", err)
	}
	for _, c := range content {
		if c.Type == "text" {
			return c.Text, nil
		}
	}
	return "", fmt.Errorf("no text content in response")
}

// writeGraphResult forwards the tool's JSON verbatim, or maps a transport error / an
// "error: ..." tool message to a JSON error response.
func (s *server) writeGraphResult(w http.ResponseWriter, text string, err error) {
	if err != nil {
		writeJSONError(w, http.StatusServiceUnavailable, err.Error())
		return
	}
	if strings.HasPrefix(text, "error:") {
		writeJSONError(w, http.StatusBadGateway, strings.TrimSpace(strings.TrimPrefix(text, "error:")))
		return
	}
	w.Header().Set("Content-Type", "application/json")
	_, _ = w.Write([]byte(text))
}

// graphMaxResults reads a bounded max_results from the query (1..200), else `def`.
func graphMaxResults(r *http.Request, def int) int {
	if v := r.URL.Query().Get("max_results"); v != "" {
		if n, err := strconv.Atoi(v); err == nil && n > 0 {
			if n > 200 {
				n = 200
			}
			return n
		}
	}
	return def
}

// GET /api/graph/hubs?project=&max_results= — a project's most-connected symbols.
func (s *server) handleGraphHubs(w http.ResponseWriter, r *http.Request) {
	project := r.URL.Query().Get("project")
	if project == "" {
		writeJSONError(w, http.StatusBadRequest, "project required")
		return
	}
	text, err := s.mcpText(r, "index_graph_hubs", map[string]any{
		"project":     project,
		"max_results": graphMaxResults(r, 20),
	})
	s.writeGraphResult(w, text, err)
}

// GET /api/graph/surprising?project=&max_results=&judge= — high-similarity, graph-far pairs.
func (s *server) handleGraphSurprising(w http.ResponseWriter, r *http.Request) {
	project := r.URL.Query().Get("project")
	if project == "" {
		writeJSONError(w, http.StatusBadRequest, "project required")
		return
	}
	args := map[string]any{"project": project, "max_results": graphMaxResults(r, 20)}
	if r.URL.Query().Get("judge") == "true" {
		args["judge"] = true
	}
	text, err := s.mcpText(r, "index_graph_surprising", args)
	s.writeGraphResult(w, text, err)
}

// GET /api/graph/neighbors?project=&node=&max_results= — a node's incident edges.
func (s *server) handleGraphNeighbors(w http.ResponseWriter, r *http.Request) {
	project := r.URL.Query().Get("project")
	node := r.URL.Query().Get("node")
	if project == "" || node == "" {
		writeJSONError(w, http.StatusBadRequest, "project and node required")
		return
	}
	text, err := s.mcpText(r, "index_graph_node", map[string]any{
		"project":     project,
		"node":        node,
		"max_results": graphMaxResults(r, 50),
	})
	s.writeGraphResult(w, text, err)
}
