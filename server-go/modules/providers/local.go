package providers

import (
	"context"
	"encoding/json"
	"errors"
	"strconv"
	"strings"
)

func normalizeEndpoint(endpoint string) string {
	if !strings.Contains(endpoint, "://") {
		endpoint = "http://" + endpoint
	}
	endpoint = strings.TrimRight(endpoint, "/")
	if i := strings.Index(endpoint, "/v1"); i >= 0 && (i+3 == len(endpoint) || endpoint[i+3] == '/') {
		return endpoint[:i+3]
	}
	return endpoint + "/v1"
}
func (m *Manager) local(ctx context.Context, req Request) (object, error) {
	pos, opts := arguments(req.Arguments)
	name, endpoint, model := opts["name"], opts["endpoint"], opts["model"]
	if endpoint == "" && len(pos) > 0 {
		if strings.Contains(pos[0], ":") || strings.HasPrefix(pos[0], "http") {
			endpoint = pos[0]
		} else {
			name = pos[0]
			if len(pos) > 1 {
				endpoint = pos[1]
			}
		}
	}
	if name == "" {
		name = "local"
	}
	if endpoint == "" {
		return nil, errors.New("model.local requires endpoint")
	}
	endpoint = normalizeEndpoint(endpoint)
	slots, _ := strconv.Atoi(opts["slots"])
	window, _ := strconv.Atoi(opts["ctx"])
	if window == 0 {
		window, _ = strconv.Atoi(opts["context-window"])
	}
	available := false
	probeMessage := ""
	slotMessage := ""
	if opts["no-probe"] != "true" {
		provider := object{"name": name, "endpoint": endpoint, "provider": "openai", "auth_type": "none"}
		reply, err := m.request(ctx, provider, "GET", "/models", nil)
		if err == nil && reply.Status == 200 {
			var root object
			if json.Unmarshal(reply.Body, &root) == nil {
				for _, row := range rows(root, "data") {
					id := str(row, "id")
					if model == "" {
						model = id
					}
					available = available || model == id
				}
			}
		} else {
			probeMessage = "model discovery unavailable"
		}
		provider["endpoint"] = strings.TrimSuffix(endpoint, "/v1")
		reply, err = m.request(ctx, provider, "GET", "/slots", nil)
		if err == nil && reply.Status == 200 {
			var payload any
			if json.Unmarshal(reply.Body, &payload) == nil {
				discoveredSlots, discoveredWindow := parseSlots(payload)
				if slots <= 0 {
					slots = discoveredSlots
				}
				if window <= 0 {
					window = discoveredWindow
				}
			}
		} else {
			slotMessage = "slot discovery unavailable"
		}
	}
	if model == "" {
		return nil, errors.New("model.local could not determine model; pass --model")
	}
	if slots <= 0 {
		slots = 1
	}
	args := []string{name, endpoint, model, "--auth-type", "none", "--max-parallel", strconv.Itoa(slots), "--context-window", strconv.Itoa(window)}
	for key, value := range opts {
		if key == "name" || key == "endpoint" || key == "model" || key == "slots" || key == "ctx" || key == "context-window" || key == "no-probe" || key == "no-fallback" || key == "no-tools" {
			continue
		}
		args = append(args, "--"+key)
		if key != "default" && key != "delegate-default" {
			args = append(args, value)
		}
	}
	if opts["no-tools"] == "true" {
		args = append(args, "--tools", "off")
	}
	req.Operation = "model.add"
	req.Arguments = object{"args": args}
	if opts["no-fallback"] != "true" {
		req.Arguments["prepend_fallback"] = name
	}
	result, err := m.Manage(ctx, req)
	if err != nil {
		return nil, err
	}
	result["model_available"] = available
	if probeMessage != "" {
		result["model_probe"] = probeMessage
	}
	if slotMessage != "" {
		result["slot_probe"] = slotMessage
	}
	return result, nil
}

func parseSlots(payload any) (slots, window int) {
	if root, ok := payload.(map[string]any); ok {
		if array, ok := root["slots"].([]any); ok {
			payload = array
		} else {
			for _, key := range []string{"slots", "n_slots", "parallel"} {
				if n := int(number(root, key)); n > 0 {
					slots = n
					break
				}
			}
			for _, key := range []string{"n_ctx", "n_ctx_slot", "context_window"} {
				if n := int(number(root, key)); n > 0 {
					window = n
					break
				}
			}
			return
		}
	}
	if array, ok := payload.([]any); ok {
		slots = len(array)
		for _, value := range array {
			if row, ok := value.(map[string]any); ok {
				for _, key := range []string{"n_ctx", "n_ctx_slot", "context_window"} {
					if n := int(number(row, key)); n > 0 && (window == 0 || n < window) {
						window = n
					}
				}
			}
		}
	}
	return
}
