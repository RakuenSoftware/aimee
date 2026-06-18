package main

import (
	"context"
	"encoding/json"
	"net/http"
)

// Curated, safe-to-toggle aimee runtime settings exposed in the webchat Settings
// UI. This is an ALLOWLIST: the browser may only read/write these keys, never
// arbitrary/sensitive config (db2_url, *_key_cmd, endpoints, …). Each maps 1:1
// to an aimee config field reached via /v1/config/{get,set}; changes persist to
// aimee.yaml and take effect on the next turn (the server reloads config per
// request).
type settingField struct {
	Key   string `json:"key"`
	Label string `json:"label"`
	Type  string `json:"type"` // "bool" | "int"
	Help  string `json:"help,omitempty"`
}

var settingsAllow = []settingField{
	{Key: "autonomous", Label: "Autonomous mode", Type: "bool",
		Help: "Agent acts without per-action confirmation."},
	{Key: "cross_verify", Label: "Cross-verify", Type: "bool",
		Help: "Double-check work before reporting done."},
	{Key: "ecomode", Label: "Eco mode", Type: "bool",
		Help: "Favor cheaper / shorter reasoning."},
	{Key: "reasoning_cap_enabled", Label: "Cap reasoning by complexity", Type: "bool",
		Help: "Lower reasoning effort on simple turns."},
	{Key: "max_iterations", Label: "Max iterations", Type: "int",
		Help: "Tool-use loop ceiling per turn (0 = default)."},
}

func settingAllowed(key string) bool {
	for _, f := range settingsAllow {
		if f.Key == key {
			return true
		}
	}
	return false
}

// handleSettings serves the webchat Settings panel against aimee runtime config.
//
//	GET  /api/settings              -> {"fields":[{key,label,type,help,value}, ...]}
//	POST /api/settings {key,value}  -> set one allowlisted key
func (s *server) handleSettings(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	ctx, cancel := context.WithTimeout(r.Context(), socketCallTimeout)
	defer cancel()

	switch r.Method {
	case http.MethodGet:
		type outField struct {
			settingField
			Value interface{} `json:"value"`
		}
		out := make([]outField, 0, len(settingsAllow))
		for _, f := range settingsAllow {
			body, _ := json.Marshal(map[string]string{"key": f.Key})
			st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/config/get", body)
			var val interface{}
			if err == nil && st == http.StatusOK {
				var d struct {
					Value interface{} `json:"value"`
				}
				if json.Unmarshal(data, &d) == nil {
					val = d.Value
				}
			}
			out = append(out, outField{settingField: f, Value: val})
		}
		_ = json.NewEncoder(w).Encode(map[string]interface{}{"fields": out})

	case http.MethodPost:
		var req struct {
			Key   string      `json:"key"`
			Value interface{} `json:"value"`
		}
		if json.NewDecoder(r.Body).Decode(&req) != nil || req.Key == "" {
			writeJSONError(w, http.StatusBadRequest, "key required")
			return
		}
		if !settingAllowed(req.Key) {
			writeJSONError(w, http.StatusForbidden, "setting not allowed")
			return
		}
		body, _ := json.Marshal(map[string]interface{}{"key": req.Key, "value": req.Value})
		st, data, err := s.v1Request(ctx, http.MethodPost, "/v1/config/set", body)
		if err != nil {
			writeJSONError(w, http.StatusServiceUnavailable, "aimee-server unavailable")
			return
		}
		w.WriteHeader(st)
		_, _ = w.Write(data)

	default:
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
	}
}
