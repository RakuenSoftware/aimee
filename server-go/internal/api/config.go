package api

import (
	"encoding/json"
	"errors"
	"io"
	"net/http"
	"strings"

	appconfig "github.com/JBailes/aimee/server-go/internal/config"
)

func (s *Server) configGet(w http.ResponseWriter, _ *http.Request) {
	if s.config == nil {
		writeError(w, http.StatusServiceUnavailable, errors.New("config store unavailable"))
		return
	}
	values, err := s.config.Values()
	if err != nil {
		writeError(w, http.StatusInternalServerError, err)
		return
	}
	writeJSON(w, http.StatusOK, map[string]any{"config": values})
}

func (s *Server) configSet(w http.ResponseWriter, r *http.Request) {
	if !workflowOperator(r) {
		writeError(w, http.StatusForbidden, errors.New("administrator access required"))
		return
	}
	if s.config == nil {
		writeError(w, http.StatusServiceUnavailable, errors.New("config store unavailable"))
		return
	}
	var request struct {
		Key             string `json:"key"`
		Value           any    `json:"value"`
		PreviousVersion string `json:"previous_version,omitempty"`
	}
	decoder := json.NewDecoder(r.Body)
	decoder.DisallowUnknownFields()
	if err := decoder.Decode(&request); err != nil || request.Key == "" {
		writeError(w, http.StatusBadRequest, errors.New("expected key and value"))
		return
	}
	if err := decoder.Decode(&struct{}{}); err != io.EOF {
		writeError(w, http.StatusBadRequest, errors.New("request must contain one JSON value"))
		return
	}
	if request.Key == "trigger_rules" && request.PreviousVersion == "" {
		writeError(w, http.StatusConflict, errors.New("trigger_rules requires previous_version"))
		return
	}
	if err := s.config.SetVersioned(request.Key, request.Value, request.PreviousVersion); err != nil {
		status := http.StatusBadRequest
		if strings.Contains(err.Error(), "version conflict") {
			status = http.StatusConflict
		}
		writeError(w, status, err)
		return
	}
	if s.notify != nil {
		s.notify()
	}
	writeJSON(w, http.StatusOK, map[string]any{"ok": true, "key": request.Key, "value": request.Value})
}

func (s *Server) workflowTriggers(w http.ResponseWriter, r *http.Request) {
	if s.config == nil {
		writeJSON(w, http.StatusOK, map[string]any{
			"operator": workflowOperator(r), "editable": false, "max_concurrent": 2,
			"max_rules": appconfig.MaxTriggerRules, "triggers": []any{},
		})
		return
	}
	rules := s.triggerRequests()
	version, _ := s.config.Version("trigger_rules")
	_, registryErr := s.config.TriggerRules()
	triggers := make([]map[string]any, 0, len(rules))
	for _, rule := range rules {
		item := map[string]any{
			"source": rule.Source, "event": rule.Event, "schedule": rule.Ref,
			"mode": rule.Mode, "template": rule.Pipeline,
			"workspace": rule.Workspace, "origin": rule.Origin,
		}
		if rule.MaxSpend != 0 {
			item["max_spend_usd"] = rule.MaxSpend
		}
		if rule.Error != "" {
			item["last_error"] = rule.Error
		}
		if runtimeError := s.triggerError(rule); runtimeError != "" {
			item["last_error"] = runtimeError
		}
		triggers = append(triggers, item)
	}
	response := map[string]any{
		"operator":       workflowOperator(r),
		"editable":       workflowOperator(r),
		"max_concurrent": s.config.Int("trigger.max_concurrent", 2),
		"max_rules":      appconfig.MaxTriggerRules,
		"version":        version,
		"triggers":       triggers,
	}
	if registryErr != nil {
		// Do not disguise a malformed registry as an empty one: the browser must
		// not offer to overwrite config it could not faithfully load.
		response["registry_error"] = registryErr.Error()
	}
	writeJSON(w, http.StatusOK, response)
}
