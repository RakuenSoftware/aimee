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
	Key     string   `json:"key"`
	Label   string   `json:"label"`
	Type    string   `json:"type"` // "bool" | "int" | "enum"
	Help    string   `json:"help,omitempty"`
	Options []string `json:"options,omitempty"` // allowed values when Type=="enum"
}

var settingsAllow = []settingField{
	{Key: "autonomous", Label: "Autonomous mode", Type: "bool",
		Help: "Agent acts without per-action confirmation."},
	{Key: "cross_verify", Label: "Cross-verify", Type: "bool",
		Help: "Double-check work before reporting done."},
	{Key: "reasoning_cap_enabled", Label: "Cap reasoning by complexity", Type: "bool",
		Help: "Lower reasoning effort on simple turns."},
	{Key: "max_iterations", Label: "Max iterations", Type: "int",
		Help: "Tool-use loop ceiling per turn (0 = default)."},
	{Key: "kb_fusion_mode", Label: "Retrieval fusion mode", Type: "enum",
		Options: []string{"rrf", "static_alpha", "dynamic_alpha"},
		Help:    "How lexical + dense KB search results are blended. dynamic_alpha adapts the weight per query (boost exact-token queries); rrf is the safe default."},
	// Options are filled at request time from the installed personas (see the
	// GET handler); the static list stays empty so it never drifts from the
	// engine's persona registry.
	{Key: "default_persona", Label: "Default persona", Type: "enum",
		Help: "Persona a new session starts in when none is explicitly set (defaults to engineer)."},
}

// personaNames lists the persona names the Default-persona selector offers, from
// aimee-server's /v1/personas (built-ins + any custom personas). The bool is
// true only when the live list was actually retrieved; on failure it returns
// (["engineer"], false) so the selector still renders while callers can tell the
// list is authoritative or a fallback (update-time validation relies on this).
func (s *server) personaNames(ctx context.Context) ([]string, bool) {
	st, data, err := s.v1Request(ctx, http.MethodGet, "/v1/personas", nil)
	if err != nil || st != http.StatusOK {
		return []string{"engineer"}, false
	}
	var d struct {
		Personas []struct {
			Name string `json:"name"`
		} `json:"personas"`
	}
	if json.Unmarshal(data, &d) != nil {
		return []string{"engineer"}, false
	}
	names := make([]string, 0, len(d.Personas))
	for _, p := range d.Personas {
		if p.Name != "" {
			names = append(names, p.Name)
		}
	}
	if len(names) == 0 {
		return []string{"engineer"}, false
	}
	return names, true
}

func containsString(xs []string, v string) bool {
	for _, x := range xs {
		if x == v {
			return true
		}
	}
	return false
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
			if f.Key == "default_persona" {
				f.Options, _ = s.personaNames(ctx)
				// Always keep the currently-saved value selectable, even if the
				// persona list is a fallback or has since dropped it — otherwise
				// the dropdown would render an invalid selection or silently
				// overwrite it on the next save.
				if cur, ok := val.(string); ok && cur != "" && !containsString(f.Options, cur) {
					f.Options = append([]string{cur}, f.Options...)
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
		// default_persona is a dynamic enum: reject an unknown persona when the
		// live list is authoritative (so a direct API caller can't bypass the
		// dropdown). When the persona service is unavailable we allow the write
		// and rely on the engine resolving an unknown name to the engineer
		// fallback, rather than blocking a legitimate change on a transient outage.
		if req.Key == "default_persona" {
			name, _ := req.Value.(string)
			if names, ok := s.personaNames(ctx); ok && !containsString(names, name) {
				writeJSONError(w, http.StatusBadRequest, "unknown persona")
				return
			}
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
