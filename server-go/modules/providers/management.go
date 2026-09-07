package providers

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"math"
	"strconv"
	"strings"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	"github.com/JBailes/aimee/server-go/modules/egress"
)

// Resources is the core's existing credential and execution storage boundary.
// Provider identity, configuration, inheritance and credential-selection policy
// belong here; resource implementations do not make those decisions.
type Resources interface {
	Credential(context.Context, string, string, string, string) (string, error)
}
type Manager struct {
	store     *Store
	resources Resources
	egress    egress.Executor
	config    Config
	metadata  *Metadata
	worker    *probeWorker
}

func NewManager(store *Store, resources Resources) *Manager {
	return &Manager{store: store, resources: resources, metadata: newMetadata(store.home)}
}

type Request struct {
	Operation          string `json:"operation"`
	Arguments          object `json:"arguments"`
	Actor              string `json:"actor"`
	SecretWriteAllowed bool   `json:"secret_write_allowed"`
	credentials        *[]credentialChange
}

func (m *Manager) Handle(inv bus.ModuleInvocation, body []byte) ([]byte, bus.ModuleStatus) {
	if inv.StageID != StageManage {
		return Rules(inv, body)
	}
	if inv.Cancelled() {
		return nil, bus.ModuleStatusCancelled
	}
	var req Request
	if json.Unmarshal(body, &req) != nil || req.Operation == "" {
		return nil, bus.ModuleStatusInvalidRequest
	}
	ctx, cancel := context.WithTimeout(context.Background(), inv.Remaining(30*time.Second))
	defer cancel()
	reply, err := m.Manage(ctx, req)
	if err != nil {
		reply = object{"status": "error", "message": err.Error(), "kind": errorKind(err)}
	}
	b, err := json.Marshal(reply)
	if err != nil {
		return nil, bus.ModuleStatusInternal
	}
	return b, bus.ModuleStatusOK
}
func (m *Manager) credential(ctx context.Context, req Request, name, key string) error {
	if key == "" || strings.HasPrefix(key, "$") {
		return nil
	}
	if !req.SecretWriteAllowed {
		return errors.New("providers: storing a credential requires an attested TLS or local connection")
	}
	if len(key) >= 4096 || strings.ContainsAny(key, "\x00\r\n") {
		return errors.New("invalid credential")
	}
	if m.resources == nil {
		return errors.New("credential service unavailable")
	}
	if req.credentials == nil {
		return errors.New("credential write outside a roster transaction")
	}
	*req.credentials = append(*req.credentials, credentialChange{name: name, secret: key})
	return nil
}
func (m *Manager) Manage(ctx context.Context, req Request) (object, error) {
	if req.Operation == "metadata.download" {
		return m.downloadMetadata(ctx)
	}
	if strings.HasPrefix(req.Operation, "metadata.") {
		return m.metadata.request(req.Operation, req.Arguments)
	}
	if err := m.recoverConfig(); err != nil {
		return nil, err
	}
	if err := m.recoverCredentials(ctx); err != nil {
		return nil, err
	}
	if req.Arguments == nil {
		req.Arguments = object{}
	}
	if strings.HasPrefix(req.Operation, "provider.") && req.Operation != "provider.connections" && req.Operation != "provider.save_connection" && req.Operation != "provider.remove_connection" && req.Operation != "provider.connection_models" {
		return m.catalog(ctx, req)
	}
	if req.Operation == "model.local" {
		return m.local(ctx, req)
	}
	read := req.Operation == "provider.connections" || req.Operation == "model.list" || req.Operation == "snapshot.load"
	if req.Operation == "model.roles" || req.Operation == "model.personas" {
		pos, opts := arguments(req.Arguments)
		read = len(pos) == 1 && opts["reset"] != "true"
	}
	if req.Operation == "provider.connection_models" || req.Operation == "model.probe" {
		return m.inspect(ctx, req)
	}
	changes := []credentialChange{}
	req.credentials = &changes
	result, err := m.store.transactionBeforeCommit(!read, func(root object) (reply object, err error) {
		before := copyObject(root)
		defer func() {
			if err == nil && !read {
				queueConcurrency(root, before)
			}
		}()
		switch req.Operation {
		case "model.register_subscription":
			return subscription(root, str(req.Arguments, "vendor"))
		case "snapshot.load":
			config := copyObject(root)
			config["revision"] = revision(root)
			for _, model := range rows(config, "models") {
				model["catalog_provider_explicit"] = str(model, "catalog_provider") != ""
				model["catalog_provider"] = catalogProvider(model)
			}
			return object{"status": "ok", "config": config}, nil
		case "snapshot.save":
			expected := str(req.Arguments, "expected_revision")
			if len(rows(root, "models")) > 0 && expected != revision(root) {
				return nil, errors.New("model configuration changed; reload before saving")
			}

			config, ok := req.Arguments["config"].(map[string]any)
			if !ok {
				return nil, errors.New("config required")
			}
			models := rows(config, "models")
			if len(models) == 0 {
				models = rows(config, "agents")
			}
			if len(models) == 0 && len(rows(root, "models")) > 0 && !boolean(req.Arguments, "allow_empty", false) {
				return nil, errors.New("refusing to erase model roster without explicit removal")
			}
			// Legacy resource-plane callers round-trip only model records. Preserve the
			// independently managed connections and reapply their current authority.
			for _, model := range models {
				if old := find(rows(root, "models"), str(model, "name")); old != nil {
					for key, value := range old {
						if !nativeModelFields[key] {
							if key == "competence" && (str(model, "model") != str(old, "model") || str(model, "registration") != str(old, "registration")) {
								continue
							}
							if _, present := model[key]; !present {
								model[key] = value
							}
						}
					}
					for _, field := range []string{"context_window", "max_output"} {
						if number(old, field) != number(model, field) {
							delete(model, field+"_origin")
						}
					}
					if p := find(rows(root, "providers"), str(old, "registration")); p != nil {
						for _, field := range connectionFields {
							if field == "cli_cmd" {
								continue
							}
							if value, present := model[field]; present && fmt.Sprint(value) != fmt.Sprint(old[field]) {
								p[field] = value
							}
						}
					}
				}
				if value, present := model["catalog_provider_explicit"]; present && value == false {
					delete(model, "catalog_provider")
				}
				delete(model, "catalog_provider_explicit")
			}
			for _, model := range models {
				registration := str(model, "registration")
				if registration == "" {
					registration = str(model, "name")
				}
				model["registration"] = registration
				if p := find(rows(root, "providers"), registration); p != nil {
					if ref := str(model, "api_key"); strings.HasPrefix(ref, "$") {
						p["api_key_ref"] = ref
					}

					applyConnection(model, p)
				}
			}
			for k, v := range config {
				if k != "providers" && k != "agents" && k != "revision" {
					root[k] = v
				}
			}
			root["models"] = models
			return object{"status": "ok"}, nil
		case "provider.connections":
			out := []object{}
			for _, p := range rows(root, "providers") {
				count := 0
				for _, model := range rows(root, "models") {
					if owns(p, model) {
						count++
					}
				}
				out = append(out, object{"name": str(p, "name"), "provider": str(p, "provider"), "endpoint": str(p, "endpoint"), "auth_type": str(p, "auth_type"), "model_count": count})
			}
			return object{"status": "ok", "providers": out}, nil
		case "provider.save_connection":
			return m.saveProvider(ctx, req, root)
		case "provider.remove_connection":
			return m.removeProvider(req, root)
		case "model.list":
			out := []object{}
			available := false
			def := str(root, "default_agent")
			for _, model := range rows(root, "models") {
				view := m.modelView(model)
				out = append(out, view)
				available = available || boolean(view, "delegate_available", false)
			}
			if p := find(rows(root, "models"), def); p == nil || !boolean(p, "enabled", true) {
				def = ""
				for _, p := range rows(root, "models") {
					if boolean(p, "enabled", true) {
						def = str(p, "name")
						break
					}
				}
			}
			return object{"status": "ok", "agents": out, "default_agent": def, "default_delegate": str(root, "default_delegate"), "any_delegate_available": available}, nil
		case "model.add", "model.set", "model.remove", "model.enable", "model.disable", "model.roles", "model.personas":
			return m.modelCommand(ctx, req, root)
		default:
			return nil, fmt.Errorf("unknown providers operation %q", req.Operation)
		}
	}, func() (func(), error) { return m.commitCredentials(ctx, changes) })
	if err == nil && !read {
		err = m.recoverConfig()
		if err == nil && result != nil {
			_, err = m.store.transaction(false, func(root object) (object, error) { result["revision"] = revision(root); return nil, nil })
		}
	}
	return result, err
}
func (m *Manager) saveProvider(ctx context.Context, req Request, root object) (object, error) {
	a := req.Arguments
	name, protocol, endpoint, auth := str(a, "name"), str(a, "provider"), str(a, "endpoint"), str(a, "auth_type")
	if name == "" || protocol == "" || !validText(name, 64) || !validText(protocol, 16) || !validText(endpoint, 512) || !validText(auth, 16) {
		return nil, errors.New("provider name and type are required and must fit their limits")
	}
	if endpoint == "" && protocol != "claude" && protocol != "claude-code" {
		return nil, errors.New("provider endpoint required")
	}
	if strings.HasPrefix(endpoint, "-") {
		return nil, errors.New("provider endpoint cannot be a flag")
	}
	connections := rows(root, "providers")
	p := find(connections, name)
	create := boolean(a, "create", false)
	if create && p != nil {
		return nil, errors.New("provider name already exists")
	}
	if !create && p == nil {
		return nil, errors.New("provider not found")
	}
	if p == nil {
		if len(connections) >= maxModels {
			return nil, errors.New("maximum number of providers reached")
		}
		p = object{"name": name}
		connections = append(connections, p)
	}
	for _, model := range rows(root, "models") {
		if owns(p, model) && str(p, "provider") != protocol {
			return nil, errors.New("remove attached models before changing provider type")
		}
	}
	key := str(a, "api_key")
	if err := m.credential(ctx, req, name, key); err != nil {
		return nil, err
	}
	if strings.HasPrefix(key, "$") {
		p["api_key_ref"] = key
	} else if key != "" {
		delete(p, "api_key_ref")
	}
	p["provider"], p["endpoint"], p["auth_type"] = protocol, endpoint, auth
	for _, model := range rows(root, "models") {
		if owns(p, model) {
			applyConnection(model, p)
		}
	}
	root["providers"] = connections
	return object{"status": "ok"}, nil
}
func (m *Manager) removeProvider(req Request, root object) (object, error) {
	name := str(req.Arguments, "name")
	p := find(rows(root, "providers"), name)
	if p == nil {
		return nil, errors.New("provider not found")
	}
	for _, model := range rows(root, "models") {
		if owns(p, model) && !boolean(req.Arguments, "remove_models", false) {
			return nil, errors.New("provider has models; confirm their removal")
		}
	}
	kept := []object{}
	for _, model := range rows(root, "models") {
		if owns(p, model) {
			removeReferences(root, str(model, "name"))
			queueCleanup(root, str(model, "name"))
		} else {
			kept = append(kept, model)
		}
	}
	root["models"] = kept
	connections := []object{}
	for _, p := range rows(root, "providers") {
		if str(p, "name") != name {
			connections = append(connections, p)
		}
	}
	root["providers"] = connections
	queueCleanup(root, name)
	repairDefault(root)
	return object{"status": "ok"}, nil
}
func removeReferences(root object, name string) {
	for _, field := range []string{"default_agent", "default_delegate"} {
		if str(root, field) == name {
			delete(root, field)
		}
	}
	chain := []string{}
	if old, ok := root["fallback_chain"].([]any); ok {
		for _, v := range old {
			if s, ok := v.(string); ok && s != name {
				chain = append(chain, s)
			}
		}
	}
	root["fallback_chain"] = chain
}
func repairDefault(root object) {
	if str(root, "default_agent") == "" && len(rows(root, "models")) > 0 {
		root["default_agent"] = str(rows(root, "models")[0], "name")
	}
}
func splitCSV(v string) []string {
	out := []string{}
	for _, part := range strings.Split(v, ",") {
		if part = strings.TrimSpace(part); part != "" {
			out = append(out, part)
		}
	}
	return out
}

var defaultRoles = []string{"code", "explain", "refactor", "draft", "execute", "summarize", "format", "reason", "search"}

func arguments(a object) ([]string, map[string]string) {
	all := []string{}
	switch v := a["args"].(type) {
	case []any:
		for _, s := range v {
			if text, ok := s.(string); ok {
				all = append(all, text)
			}
		}
	case []string:
		all = v
	}
	pos := []string{}
	opts := map[string]string{}
	for i := 0; i < len(all); i++ {
		v := all[i]
		if !strings.HasPrefix(v, "--") {
			pos = append(pos, v)
			continue
		}
		v = strings.TrimPrefix(v, "--")
		if k, val, ok := strings.Cut(v, "="); ok {
			opts[k] = val
			continue
		}
		if v == "default" || v == "delegate-default" || v == "disabled" || v == "reset" || v == "no-run" || v == "no-probe" || v == "no-tools" || v == "no-fallback" {
			opts[v] = "true"
		} else if i+1 < len(all) {
			i++
			opts[v] = all[i]
		} else {
			opts[v] = ""
		}
	}
	return pos, opts
}
func (m *Manager) modelCommand(ctx context.Context, req Request, root object) (object, error) {
	pos, opts := arguments(req.Arguments)
	if len(pos) == 0 || pos[0] == "" {
		return nil, errors.New("model name required")
	}
	name := pos[0]
	models := rows(root, "models")
	model := find(models, name)
	if req.Operation == "model.add" {
		if len(pos) < 3 {
			return nil, errors.New("usage: agent add <name> <endpoint> <model>")
		}
		if !validText(name, 64) || pos[2] == "" || !validText(pos[2], 128) {
			return nil, errors.New("model id or name is empty or too long")
		}
		p := find(rows(root, "providers"), opts["registration"])
		if _, ok := opts["registration"]; ok && p == nil {
			return nil, errors.New("provider not found")
		}
		if p != nil {
			for _, field := range []string{"provider", "endpoint", "auth-type", "auth-cmd", "key"} {
				if _, ok := opts[field]; ok {
					return nil, errors.New("connection settings are inherited from the provider")
				}
			}
		}
		if p != nil && model != nil {
			return nil, errors.New("model name already exists")
		}
		if model == nil {
			if len(models) >= maxModels {
				return nil, errors.New("maximum number of agents reached")
			}
			model = object{}
			models = append(models, model)
		} else {
			for key := range model {
				delete(model, key)
			}
		}
		model["name"], model["endpoint"], model["model"], model["provider"], model["auth_type"] = name, pos[1], pos[2], "openai", "bearer"
		model["enabled"], model["max_turns"], model["max_parallel"], model["roles"], model["tools_enabled"] = opts["disabled"] != "true", -1, 3, defaultRoles, true
		if p != nil {
			applyConnection(model, p)
		}
	} else if model == nil {
		return nil, errors.New("agent not found")
	}
	if req.Operation == "model.set" && str(model, "registration") != "" {
		for _, field := range []string{"provider", "endpoint", "auth-type", "auth-cmd", "key"} {
			if _, ok := opts[field]; ok {
				return nil, errors.New("edit connection settings through the provider connection")
			}
		}
	}

	switch req.Operation {
	case "model.remove":
		kept := []object{}
		for _, item := range models {
			if str(item, "name") != name {
				kept = append(kept, item)
			}
		}
		root["models"] = kept
		removeReferences(root, name)
		repairDefault(root)
		return object{"status": "ok", "name": name, "removed": true}, nil
	case "model.enable", "model.disable":
		model["enabled"] = req.Operation == "model.enable"
	case "model.roles", "model.personas":
		field := strings.TrimPrefix(req.Operation, "model.")
		if len(pos) > 1 {
			model[field] = splitCSV(pos[1])
		} else if opts["reset"] == "true" {
			if field == "roles" {
				model[field] = defaultRoles
			} else {
				model[field] = []string{"all"}
			}
		}
	default:
		if v, ok := opts["model"]; ok && v != str(model, "model") {
			delete(model, "competence")
		}
		if raw, ok := opts["competence"]; ok {
			var evidence map[string]any
			if json.Unmarshal([]byte(raw), &evidence) != nil || evidence == nil {
				return nil, errors.New("competence must be a JSON object of role assessments")
			}
			model["competence"] = evidence
			delete(model, "competence_model")
		}
		for option, field := range map[string]string{"model": "model", "endpoint": "endpoint", "provider": "provider", "auth-type": "auth_type", "auth-cmd": "auth_cmd"} {
			if v, ok := opts[option]; ok {
				model[field] = v
			}
		}
		for option, field := range map[string]string{"cost-tier": "cost_tier", "max-turns": "max_turns", "max-parallel": "max_parallel", "max-tokens": "max_tokens", "timeout": "timeout_ms", "context-window": "context_window", "ctx": "context_window", "max-output": "max_output"} {
			if v, ok := opts[option]; ok {
				n, _ := strconv.Atoi(v)
				delete(model, field+"_origin")
				if (field == "context_window" || field == "max_output") && n <= 0 {
					delete(model, field)
				} else {
					model[field] = n
				}
			}
		}
		for option, field := range map[string]string{"price-in": "price_in_per_mtok", "price-out": "price_out_per_mtok", "price-cached": "price_cached_per_mtok"} {
			if v, ok := opts[option]; ok {
				if v == "" {
					delete(model, field)
				} else if n, err := strconv.ParseFloat(v, 64); err == nil && n >= 0 && n <= 1e12 && !math.IsInf(n, 0) && !math.IsNaN(n) {
					model[field] = n
				}
			}
		}
		for option, field := range map[string]string{"tools": "tools_enabled", "enabled": "enabled", "primary-only": "primary_only"} {
			if v, ok := opts[option]; ok {
				model[field] = v == "on" || v == "true" || v == "1"
				if option == "tools" {
					model["inject_respond_tool"] = model[field]
				}
			}
		}
		for _, field := range []string{"roles", "personas", "exec-roles"} {
			if v, ok := opts[field]; ok {
				model[strings.ReplaceAll(field, "-", "_")] = splitCSV(v)
			}
		}
		if key := opts["key"]; key != "" {
			if err := m.credential(ctx, req, name, key); err != nil {
				return nil, err
			}
			if strings.HasPrefix(key, "$") {
				model["api_key"] = key
			} else {
				delete(model, "api_key")
			}
		}
	}
	if str(model, "provider") == "codex" {
		model["provider"] = "chatgpt"
		if _, ok := opts["auth-type"]; !ok {
			model["auth_type"] = "codex-oauth"
		}
	}
	if p := str(model, "provider"); p == "claude" || p == "claude-code" {
		model["backend"] = "tmux-cli"
		model["cli_kind"] = p
		model["cli_cmd"] = "claude --model " + str(model, "model")
		model["session_reuse"] = true
		model["endpoint"] = ""
	}
	if opts["default"] == "true" {
		root["default_agent"] = name
	}
	if opts["delegate-default"] == "true" {
		root["default_delegate"] = name
	}
	if err := normalizeModel(model); err != nil {
		return nil, err
	}
	root["models"] = models
	if prepend := str(req.Arguments, "prepend_fallback"); prepend != "" {
		chain := []string{prepend}
		if old, ok := root["fallback_chain"].([]any); ok {
			for _, v := range old {
				if value, ok := v.(string); ok && value != prepend {
					chain = append(chain, value)
				}
			}
		}
		root["fallback_chain"] = chain
	}
	repairDefault(root)
	view := m.modelView(model)
	view["status"] = "ok"
	if (req.Operation == "model.roles" || req.Operation == "model.personas") && len(pos) == 1 && opts["reset"] != "true" {
		view["read_only"] = true
	}
	return view, nil
}
func modelView(model object) object {
	out := copyObject(model)
	for _, key := range []string{"api_key", "auth_cmd"} {
		delete(out, key)
	}
	if str(out, "provider") == "" {
		out["provider"] = "openai"
	}
	if str(out, "auth_type") == "" {
		out["auth_type"] = "bearer"
	}
	out["enabled"] = boolean(model, "enabled", true)
	out["tools_enabled"] = boolean(model, "tools_enabled", true)
	out["primary_only"] = boolean(model, "primary_only", false)
	out["delegate_available"] = boolean(out, "enabled", true) && !boolean(out, "primary_only", false)
	for key, value := range map[string]any{"max_parallel": 3, "max_turns": -1, "cost_tier": 0, "roles": defaultRoles, "personas": []string{}} {
		if _, ok := out[key]; !ok {
			out[key] = value
		}
	}
	out["catalog_provider"] = catalogProvider(out)
	out["model_ref"] = str(out, "catalog_provider") + ":" + str(out, "model")
	out["price_overridden"] = false
	for _, field := range []string{"in", "out", "cached"} {
		key := "price_" + field + "_per_mtok"
		_, declared := model[key]
		out["price_"+field+"_declared"] = declared
		if declared {
			out["price_overridden"] = true
			out["price_base_"+field+"_per_mtok"] = model[key]
		}
	}
	for _, field := range []string{"context_window", "max_output"} {
		n := number(model, field)
		out["effective_"+field] = n
		out[field+"_source"] = "unknown"
		if n > 0 {
			out[field+"_source"] = "declared"
			if origin := str(model, field+"_origin"); origin != "" {
				out[field+"_source"] = origin
			}
		}
	}
	return out
}

func errorKind(err error) string {
	message := err.Error()
	if strings.Contains(message, "not found") {
		return "not_found"
	}
	if strings.Contains(message, "attested") {
		return "permission_denied"
	}
	for _, term := range []string{"unavailable", "invalid provider/model configuration", "must be an array", "invalid saved provider", "duplicate saved provider", "configuration changed", "permission denied", "no such file"} {
		if strings.Contains(message, term) {
			return "unavailable"
		}
	}
	return "invalid_argument"
}
