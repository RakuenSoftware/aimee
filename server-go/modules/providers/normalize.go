package providers

import (
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"strings"
)

func localEndpoint(endpoint string) bool {
	h := endpointHost(endpoint)
	ip := net.ParseIP(h)
	return h == "localhost" || ip != nil && (ip.IsLoopback() || ip.IsPrivate() || ip.IsUnspecified()) || strings.HasPrefix(endpoint, "unix://") || strings.HasPrefix(endpoint, "/")
}
func normalizeModel(model object) error {
	for field, limit := range map[string]int{"name": 64, "registration": 64, "provider": 16, "auth_type": 16, "model": 128, "endpoint": 512, "catalog_provider": 32, "cli_kind": 16, "cli_cmd": 256, "auth_cmd": 512} {
		if !validText(str(model, field), limit) {
			return fmt.Errorf("%s exceeds runtime capacity or contains a control character", field)
		}
	}
	for _, field := range []string{"roles", "personas", "exec_roles"} {
		if values, ok := model[field]; ok {
			raw, _ := json.Marshal(values)
			var items []string
			if json.Unmarshal(raw, &items) != nil || len(items) > 16 {
				return fmt.Errorf("invalid %s list", field)
			}
			for _, item := range items {
				if !validText(item, 32) {
					return fmt.Errorf("%s entry exceeds runtime capacity", field)
				}
			}
		}
	}
	for _, field := range []string{"enabled", "tools_enabled", "primary_only", "session_reuse"} {
		if v, ok := model[field]; ok {
			switch v := v.(type) {
			case bool:
			case float64:
				if v != 0 && v != 1 {
					return fmt.Errorf("invalid %s", field)
				}
				model[field] = v == 1
			case int:
				if v != 0 && v != 1 {
					return fmt.Errorf("invalid %s", field)
				}
				model[field] = v == 1
			default:
				return fmt.Errorf("invalid %s", field)
			}
		}
	}
	if str(model, "provider") == "" {
		model["provider"] = "openai"
	}
	if str(model, "provider") == "openai" {
		host := endpointHost(str(model, "endpoint"))
		mini := family(strings.ToLower(str(model, "model")), "minimax")
		for _, domain := range []string{"minimax.io", "minimaxi.com", "minimax.com"} {
			mini = mini || host == domain || strings.HasSuffix(host, "."+domain)
		}
		if mini {
			model["provider"] = "minimax"
		}
	}
	if str(model, "auth_type") == "" {
		model["auth_type"] = "bearer"
	}
	if str(model, "backend") == "cli-stdio" {
		model["backend"] = "provider-cli"
	}
	if str(model, "backend") == "provider-cli" && str(model, "cli_kind") == "claude" {
		model["backend"] = "tmux-cli"
		if str(model, "provider") == "openai" {
			model["provider"] = "claude"
		}
		if str(model, "auth_type") == "bearer" {
			model["auth_type"] = "none"
		}
		if cmd := str(model, "cli_cmd"); cmd == "" || cmd == "claude-p" {
			model["cli_cmd"] = "claude"
		}
		delete(model, "cli_kind")
	}
	if _, ok := model["primary_only"]; !ok {
		model["primary_only"] = str(model, "backend") == "tmux-cli" && (str(model, "provider") == "claude" || str(model, "cli_kind") == "claude")
	}
	if str(model, "backend") == "provider-cli" && str(model, "cli_kind") == "mistral-plan" {
		model["cost_tier"] = 0
	}
	if mw, ok := model["middleware"].(map[string]any); ok {
		if _, ok := model["context_window"]; !ok {
			if v, ok := mw["context_window"]; ok {
				model["context_window"] = v
			}
		}
	}
	if number(model, "context_window") > 0 && number(model, "max_output") > number(model, "context_window") {
		return errors.New("max output cannot exceed the context window")
	}
	if scope := str(model, "max_scope"); scope != "" && scope != "bounded" && scope != "whole_task" {
		return errors.New("unknown max_scope; use bounded or whole_task")
	}
	if _, ok := model["inject_respond_tool"]; !ok {
		p := str(model, "provider")
		model["inject_respond_tool"] = boolean(model, "tools_enabled", true) && (p == "ollama" || p == "llama_native" || p == "llama-eval" || str(model, "auth_type") == "none" && localEndpoint(str(model, "endpoint")))
	}
	for key, v := range map[string]any{"enabled": true, "max_tokens": 8192, "max_turns": -1} {
		if _, ok := model[key]; !ok {
			model[key] = v
		}
	}
	return nil
}

func expandModels(models []object) ([]object, error) {
	out := []object{}
	seen := map[string]bool{}
	for _, original := range models {
		expanded := []object{original}
		if values, exists := original["models"]; exists {
			if str(original, "model") != "" {
				return nil, errors.New("registration cannot set both model and models")
			}
			ids := []string{}
			switch v := values.(type) {
			case string:
				p := profile(catalogProvider(original))
				if v != "auto" || p == nil {
					return nil, errors.New("provider has no curated auto model list")
				}
				ids, _ = p["routable_models"].([]string)
			case []any:
				for _, value := range v {
					id, ok := value.(string)
					if !ok || id == "" {
						return nil, errors.New("invalid registration model id")
					}
					ids = append(ids, id)
				}
			default:
				return nil, errors.New("models must be a nonempty array or auto")
			}
			if len(ids) == 0 {
				return nil, errors.New("empty registration models")
			}
			expanded = nil
			for _, id := range ids {
				model := copyObject(original)
				delete(model, "models")
				model["derive_cost_tier"] = true
				model["name"], model["model"], model["registration"] = str(original, "name")+":"+id, id, str(original, "name")
				expanded = append(expanded, model)
			}
		}
		for _, model := range expanded {
			if err := normalizeModel(model); err != nil {
				return nil, err
			}
			name := str(model, "name")
			if name == "" || seen[name] {
				return nil, errors.New("empty or duplicate model name")
			}
			seen[name] = true
			out = append(out, model)
		}
	}
	if len(out) > maxModels {
		return nil, errors.New("model registration exceeds runtime capacity")
	}
	return out, nil
}

// Runtime defaults are derived in Go before the native ABI is hydrated.
func (s *Store) defaults(root object) object {
	catalog := newMetadata(s.home)
	for _, model := range rows(root, "models") {
		cap, _ := catalog.request("metadata.show", object{"provider": catalogProvider(model), "model": str(model, "model")})
		meta, _ := cap["model"].(map[string]any)
		flags := int(number(meta, "flags_mask"))
		if boolean(model, "derive_cost_tier", false) {
			price := number(meta, "cost_in_per_mtok")
			tier := 4
			for i, threshold := range []float64{0.5, 1.5, 3.5, 7.5} {
				if price < threshold {
					tier = i
					break
				}
			}
			if price > 0 {
				model["cost_tier"] = tier
			}
			delete(model, "derive_cost_tier")
		}
		published, _ := catalog.request("metadata.published", object{"provider": catalogProvider(model), "model": str(model, "model")})
		known, _ := published["model"].(map[string]any)
		for _, field := range []string{"context_window", "max_output"} {
			if number(model, field) <= 0 && number(known, field) > 0 {
				model[field] = known[field]
				model[field+"_origin"] = "resolved"
			}
		}
		if _, ok := model["timeout_ms"]; !ok {
			model["timeout_ms"] = 180000
			if flags&1 != 0 {
				model["timeout_ms"] = 600000
			}
		}
		if _, ok := model["tools_enabled"]; !ok {
			model["tools_enabled"] = meta == nil || flags&2 != 0
		}
		if _, ok := model["max_parallel"]; !ok {
			model["max_parallel"] = 3
			if str(model, "provider") == "mistral" {
				model["max_parallel"] = 2
			}
		}
	}
	return root
}
