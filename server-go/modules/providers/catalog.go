package providers

import (
	"context"
	"errors"
	"fmt"
	"strings"

	configclient "github.com/JBailes/aimee/server-go/config"
)

type Config interface {
	StringValue(string) (string, bool, error)
	Set(string, any) error
	SetModelConcurrency(configclient.ModelConcurrencyMutation) error
	RemoveModelConcurrency(string) error
}

func (m *Manager) SetConfig(c Config) { m.config = c }

func (m *Manager) profileView(ctx context.Context, p object) (object, error) {
	out := copyObject(p)
	envs, _ := out["env_vars"].([]any)
	out["env_var"] = ""
	if len(envs) > 0 {
		out["env_var"] = envs[0]
	}
	key, err := m.resolveKey(ctx, p)
	if err != nil {
		return nil, err
	}
	out["available"] = str(p, "auth_type") == "none" || key != ""
	out["configured"] = key != ""
	return out, nil
}

func (m *Manager) catalog(ctx context.Context, req Request) (object, error) {
	name := str(req.Arguments, "name")
	switch req.Operation {
	case "provider.profiles":
		return object{"status": "ok", "providers": profiles}, nil
	case "provider.classify":
		status := int(number(req.Arguments, "http_status"))
		body := str(req.Arguments, "body")
		reason := 0
		if name == "openrouter" && (status == 400 || status == 403 || status == 404) && (strings.Contains(body, "No endpoints found") || strings.Contains(body, "data policy") || strings.Contains(body, "privacy policy")) {
			reason = 11
		}
		if name == "ollama" && (status == 400 || status == 500) && (strings.Contains(body, "num_ctx") || strings.Contains(body, "prompt too long")) {
			reason = 8
		}
		return object{"status": "ok", "reason": reason}, nil
	case "provider.get", "provider.set":
		if m.config == nil {
			return nil, errors.New("config service unavailable")
		}
		if req.Operation == "provider.set" {
			if name == "" || !validText(name, 16) {
				return nil, errors.New("provider name required (maximum 15 bytes)")
			}
			valid := profile(name) != nil || name == "claude" || name == "codex" || name == "chatgpt"
			_, err := m.store.transaction(false, func(root object) (object, error) {
				valid = valid || find(rows(root, "models"), name) != nil || find(rows(root, "providers"), name) != nil
				return nil, nil
			})
			if err != nil {
				return nil, err
			}
			if !valid {
				return nil, errors.New("unknown provider")
			}
			if err = m.config.Set("provider", name); err != nil {
				return nil, err
			}
		} else {
			var err error
			name, _, err = m.config.StringValue("provider")
			if err != nil {
				return nil, err
			}
			if name == "" {
				name = "claude"
			}
		}
		return object{"status": "ok", "provider": name}, nil
	case "provider.list":
		selected := ""
		if m.config != nil {
			var err error
			selected, _, err = m.config.StringValue("provider")
			if err != nil {
				return nil, err
			}
		}
		all, available := boolean(req.Arguments, "all", false), boolean(req.Arguments, "available_only", false)
		out := []object{}
		for _, p := range profiles {
			view, err := m.profileView(ctx, p)
			if err != nil {
				return nil, err
			}
			if available && !boolean(view, "available", false) {
				continue
			}
			if !all && !available && !boolean(view, "configured", false) && str(p, "name") != selected {
				continue
			}
			delete(view, "configured")
			out = append(out, view)
		}
		return object{"status": "ok", "providers": out, "all": all, "available_only": available, "json": boolean(req.Arguments, "json", false)}, nil
	case "provider.show":
		if p := profile(name); p != nil {
			view, err := m.profileView(ctx, p)
			return object{"status": "ok", "provider": view}, err
		}
		return nil, errors.New("provider not found")
	case "provider.models", "provider.test":
		p := profile(name)
		if p == nil {
			return nil, errors.New("provider not found")
		}
		p = copyObject(p)
		p["provider"], p["endpoint"] = name, str(p, "base_url")
		out, err := m.discover(ctx, p)
		if err != nil {
			return nil, err
		}
		out["provider"] = name
		out["json"] = boolean(req.Arguments, "json", false)
		if req.Operation == "provider.test" {
			out["message"] = fmt.Sprintf("%s: ok (%d models)", name, len(rows(out, "details")))
		}
		return out, nil
	}
	return nil, fmt.Errorf("unknown catalog operation %s", req.Operation)
}

func subscription(root object, vendor string) (object, error) {
	if vendor != "claude" && vendor != "codex" {
		return nil, errors.New("vendor must be claude or codex")
	}
	name := vendor
	models := rows(root, "models")
	model := find(models, name)
	if model == nil {
		if len(models) >= maxModels {
			return nil, errors.New("maximum number of models reached")
		}
		model = object{}
		models = append(models, model)
	}
	// A poll can be retried after the process restarts. Preserve operator settings
	// once this account is registered instead of resetting them on every poll.
	if boolean(model, "is_server_hosted", false) {
		return object{"status": "ok", "agent": name}, nil
	}
	for k := range model {
		delete(model, k)
	}
	for k, v := range (object{"name": name, "registration": name, "enabled": true, "tools_enabled": true, "max_turns": -1, "max_parallel": 3, "max_tokens": 8192, "timeout_ms": 600000, "is_server_hosted": true, "roles": defaultRoles}) {
		model[k] = v
	}
	if vendor == "codex" {
		model["provider"], model["auth_type"], model["endpoint"], model["model"], model["context_window"], model["cost_tier"] = "chatgpt", "codex-oauth", "https://chatgpt.com/backend-api/codex", "gpt-5.5", 272000, 0
	} else {
		model["provider"], model["auth_type"], model["backend"], model["cli_cmd"], model["session_reuse"], model["primary_only"], model["context_window"], model["cost_tier"] = "claude", "none", "tmux-cli", "claude", true, true, 200000, 1
	}
	p := object{"name": name}
	for _, field := range connectionFields {
		if v, ok := model[field]; ok {
			p[field] = v
		}
	}
	connections := rows(root, "providers")
	if old := find(connections, name); old != nil {
		for k := range old {
			delete(old, k)
		}
		for k, v := range p {
			old[k] = v
		}
	} else {
		connections = append(connections, p)
	}
	root["providers"], root["models"] = connections, models
	repairDefault(root)
	return object{"status": "ok", "agent": name}, nil
}

func catalogProvider(model object) string {
	if v := str(model, "catalog_provider"); v != "" {
		return v
	}
	host := endpointHost(str(model, "endpoint"))
	for _, item := range [][2]string{{"minimax.io", "minimax"}, {"minimaxi.com", "minimax"}, {"minimax.com", "minimax"}, {"kimi.com", "moonshotai"}, {"moonshot.cn", "moonshotai"}, {"moonshot.ai", "moonshotai"}} {
		if host == item[0] || strings.HasSuffix(host, "."+item[0]) {
			return item[1]
		}
	}
	id := strings.ToLower(str(model, "model"))
	ns, _, _ := strings.Cut(id, "/")
	if ns == "minimax" || family(id, "minimax") {
		return "minimax"
	}
	if ns == "moonshotai" || ns == "moonshot" || ns == "kimi" || family(id, "kimi") {
		return "moonshotai"
	}
	switch p := str(model, "provider"); p {
	case "claude", "claude-code":
		return "anthropic"
	case "chatgpt", "codex", "":
		return "openai"
	default:
		return p
	}
}
func family(id, prefix string) bool {
	return id == prefix || strings.HasPrefix(id, prefix) && strings.ContainsRune("-_./: ", rune(id[len(prefix)]))
}
