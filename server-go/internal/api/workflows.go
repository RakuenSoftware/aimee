package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"sort"

	"github.com/JBailes/aimee/server-go/internal/wfe"
)

type workflowBinding struct {
	Input    string `json:"input"`
	Producer string `json:"producer"`
	Output   string `json:"output"`
}

type workflowNode struct {
	ID       string            `json:"id"`
	Block    string            `json:"block"`
	Custom   bool              `json:"custom"`
	Produces string            `json:"produces"`
	In       []workflowBinding `json:"in"`
	Next     string            `json:"next,omitempty"`
	OnPass   string            `json:"on_pass,omitempty"`
	OnFail   string            `json:"on_fail,omitempty"`
	Params   map[string]any    `json:"params,omitempty"`
}

type workflowGraph struct {
	Name     string         `json:"name"`
	Start    string         `json:"start"`
	Enforced bool           `json:"enforced,omitempty"`
	Nodes    []workflowNode `json:"nodes"`
}

func graphReport(report wfe.DefinitionReport, catalog map[string]wfe.BlockDefinition) map[string]any {
	graph := workflowGraph{Name: report.Def.Name, Start: report.Def.Start,
		Enforced: report.Def.Enforced, Nodes: make([]workflowNode, 0, len(report.Def.Nodes))}
	if graph.Start == "" && len(report.Def.Nodes) > 0 {
		graph.Start = report.Def.Nodes[0].ID
	}
	for _, node := range report.Def.Nodes {
		block := catalog[node.Block]
		out := workflowNode{ID: node.ID, Block: node.Block, Custom: block.Custom,
			Produces: block.Produces, Next: node.Next, OnPass: node.OnPass, OnFail: node.OnFail,
			Params: node.Params, In: make([]workflowBinding, 0, len(node.In))}
		names := make([]string, 0, len(node.In))
		for name := range node.In {
			names = append(names, name)
		}
		sort.Strings(names)
		for _, name := range names {
			producer, output, _ := splitBinding(node.In[name])
			out.In = append(out.In, workflowBinding{Input: name, Producer: producer, Output: output})
		}
		graph.Nodes = append(graph.Nodes, out)
	}
	result := map[string]any{"valid": report.Valid, "name": report.Name,
		"version": report.Version, "canonical": report.Canonical, "def": graph}
	if report.Error != "" {
		result["error"] = report.Error
	}
	return result
}

func splitBinding(binding string) (string, string, bool) {
	for i := len(binding) - 1; i >= 0; i-- {
		if binding[i] == '.' && i > 0 && i < len(binding)-1 {
			return binding[:i], binding[i+1:], true
		}
	}
	return "", "", false
}

func requireWorkflowAdmin(w http.ResponseWriter, r *http.Request) bool {
	if !workflowOperator(r) {
		writeError(w, http.StatusForbidden, errors.New("administrator access required"))
		return false
	}
	return true
}

func (s *Server) workflowBlocks(w http.ResponseWriter, r *http.Request) {
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	blocks, err := registry.Blocks()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{
		"blocks": blocks, "editable": workflowOperator(r),
	})
}

func (s *Server) workflowBlockPut(w http.ResponseWriter, r *http.Request) {
	if !requireWorkflowAdmin(w, r) {
		return
	}
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	var block wfe.BlockDefinition
	decoder := jsonDecoder(r.Body)
	if err := decoder.Decode(&block); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, errors.New("request must contain one JSON value"))
		return
	}
	block.Name = r.PathValue("name")
	if err := registry.SaveDelegateBlock(block); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	s.workflowBlocks(w, r)
}

func (s *Server) workflowBlockDelete(w http.ResponseWriter, r *http.Request) {
	if !requireWorkflowAdmin(w, r) {
		return
	}
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	if err := registry.DeleteDelegateBlock(r.PathValue("name")); err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	s.workflowBlocks(w, r)
}

func (s *Server) workflowDefinitions(w http.ResponseWriter, _ *http.Request) {
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	rows, err := registry.List()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"defs": rows})
}

func (s *Server) workflowDefinition(w http.ResponseWriter, r *http.Request) {
	registry, err := s.workflowRegistry()
	if err != nil {
		writeError(w, http.StatusServiceUnavailable, err)
		return
	}
	report, err := registry.Report(r.PathValue("name"))
	if err != nil {
		writeError(w, http.StatusNotFound, errors.New("workflow not found"))
		return
	}
	catalog, _ := registry.Catalog()
	writeJSON(w, http.StatusOK, graphReport(report, catalog))
}

type workflowYAMLRequest struct {
	Name            string `json:"name"`
	YAML            string `json:"yaml"`
	PreviousVersion string `json:"prev_version"`
}

func decodeWorkflowRequest(r *http.Request) (workflowYAMLRequest, error) {
	var request workflowYAMLRequest
	decoder := jsonDecoder(r.Body)
	if err := decoder.Decode(&request); err != nil {
		return request, err
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		return request, errors.New("request must contain one JSON value")
	}
	if request.YAML == "" {
		return request, errors.New("yaml is required")
	}
	return request, nil
}

func jsonDecoder(body io.Reader) *json.Decoder {
	d := json.NewDecoder(body)
	d.DisallowUnknownFields()
	return d
}

func (s *Server) workflowValidate(w http.ResponseWriter, r *http.Request) {
	request, err := decodeWorkflowRequest(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	registry, regErr := s.workflowRegistry()
	if regErr != nil {
		writeError(w, http.StatusServiceUnavailable, regErr)
		return
	}
	report := registry.Validate([]byte(request.YAML))
	catalog, _ := registry.Catalog()
	writeJSON(w, http.StatusOK, graphReport(report, catalog))
}

func (s *Server) workflowSave(w http.ResponseWriter, r *http.Request) {
	if !requireWorkflowAdmin(w, r) {
		return
	}
	request, err := decodeWorkflowRequest(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, err)
		return
	}
	registry, regErr := s.workflowRegistry()
	if regErr != nil {
		writeError(w, http.StatusServiceUnavailable, regErr)
		return
	}
	report, err := registry.Save(request.Name, []byte(request.YAML), request.PreviousVersion)
	if err != nil {
		var conflict *wfe.VersionConflictError
		if errors.As(err, &conflict) {
			writeJSON(w, http.StatusConflict, map[string]any{"error": "version conflict", "current_version": conflict.Current})
			return
		}
		writeError(w, http.StatusBadRequest, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"name": report.Name, "version": report.Version})
}
