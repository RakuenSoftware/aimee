package providers

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net/url"
	"strings"

	"github.com/JBailes/aimee/server-go/modules/egress"
)

func (m *Manager) SetEgress(client egress.Executor) { m.egress = client }
func (m *Manager) request(ctx context.Context, p object, method, suffix string, body []byte) (egress.HTTPResponse, error) {
	if m.egress == nil {
		return egress.HTTPResponse{}, errors.New("provider network service unavailable")
	}
	endpoint := str(p, "endpoint")
	if !strings.Contains(endpoint, "://") {
		endpoint = "http://" + endpoint
	}
	endpoint = strings.TrimRight(endpoint, "/")
	for _, route := range []string{"/chat/completions", "/messages", "/responses"} {
		endpoint = strings.TrimSuffix(endpoint, route)
	}
	target := endpoint + suffix
	parsed, err := url.Parse(target)
	if err != nil || parsed.Hostname() == "" {
		return egress.HTTPResponse{}, errors.New("invalid provider endpoint")
	}
	auth := str(p, "auth_type")
	if auth == "" || auth == "api_key" {
		auth = "bearer"
	}
	if str(p, "provider") == "anthropic" && auth == "bearer" {
		auth = "x-api-key"
	}
	credential := auth != "none"
	handle := ""
	if credential {
		handle = "provider"
	}
	req := egress.HTTPRequest{Request: egress.Request{TargetURL: target, Purpose: "provider", Method: method, CredentialPresent: credential, RequestSHA256: egress.RequestDigest(method, target, body, credential)}, Headers: map[string]string{"Content-Type": "application/json"}, Body: body, CredentialHandle: handle, CredentialScope: auth, MaxResponseBytes: 1 << 20, TimeoutMS: 5000}
	if n := number(p, "max_response_bytes"); n > 0 {
		req.MaxResponseBytes = int64(n)
	}
	if str(p, "provider") == "anthropic" {
		req.Headers["anthropic-version"] = "2023-06-01"
	}
	if credential {
		if m.resources == nil {
			return egress.HTTPResponse{}, errors.New("credential storage unavailable")
		}
		key, err := m.resolveKey(ctx, p)
		if err != nil {
			return egress.HTTPResponse{}, err
		}
		if key != "" {
			sealer, ok := m.egress.(interface {
				SealProviderCredential(context.Context, uint64, string, string, string, []byte) (*egress.CredentialEnvelope, error)
			})
			if !ok {
				return egress.HTTPResponse{}, errors.New("credential sealing unavailable")
			}
			secret := []byte(key)
			req.Credential, err = sealer.SealProviderCredential(ctx, 0, target, str(p, "name"), auth, secret)
			clear(secret)
			if err != nil {
				return egress.HTTPResponse{}, err
			}
			req.CredentialResource = str(p, "name")
		} else {
			req.CredentialPresent = false
			req.CredentialHandle = ""
			req.CredentialScope = ""
			req.RequestSHA256 = egress.RequestDigest(method, target, body, false)
		}
	}
	return m.egress.Do(ctx, 0, req)
}
func (m *Manager) inspect(ctx context.Context, req Request) (object, error) {
	var provider, model object
	_, err := m.store.transaction(false, func(root object) (object, error) {
		if req.Operation == "provider.connection_models" {
			provider = find(rows(root, "providers"), str(req.Arguments, "name"))
		} else {
			pos, _ := arguments(req.Arguments)
			if len(pos) == 0 {
				return nil, errors.New("model.probe requires name")
			}
			model = find(rows(root, "models"), pos[0])
			if model == nil {
				return nil, errors.New("agent not found")
			}
			provider = find(rows(root, "providers"), str(model, "registration"))
		}
		if provider == nil {
			return nil, errors.New("provider not found")
		}
		provider = copyObject(provider)
		return nil, nil
	})
	if err != nil {
		return nil, err
	}
	if model != nil && str(model, "backend") != "" && str(model, "backend") != "http" {
		backend := str(model, "backend")
		_, opts := arguments(req.Arguments)
		result := object{"status": "ok", "name": str(model, "name"), "backend": backend, "provider": str(model, "provider"), "models_status": -1, "models_probe_skipped": true, "slots_probe_skipped": true, "slots_source": "config", "slots": number(model, "max_parallel"), "context_window": number(model, "context_window"), "execution_tested": false}
		if opts["no-run"] == "true" {
			return result, nil
		}
		result["execution_tested"], result["execution_ok"] = true, false
		if backend != "tmux-cli" && backend != "provider-cli" {
			result["execution_error"] = "unsupported model backend"
			return result, nil
		}
		if m.worker == nil {
			result["execution_error"] = "CLI diagnostic worker unavailable"
			return result, nil
		}
		reply, err := m.worker.probe(ctx, str(model, "name"))
		if err != nil {
			result["execution_error"] = "CLI diagnostic worker lost; restart the providers module"
		} else {
			for k, v := range reply {
				result[k] = v
			}
		}
		return result, nil
	}
	listed, fetchErr := m.discover(ctx, provider)
	if req.Operation == "provider.connection_models" {
		return listed, fetchErr
	}
	ids := []string{}
	if listed != nil {
		ids, _ = listed["models"].([]string)
	}
	responseStatus := 200
	if fetchErr != nil {
		responseStatus = -1
	}
	available := false
	for _, id := range ids {
		if id == str(model, "model") {
			available = true
		}
	}
	_, opts := arguments(req.Arguments)
	result := object{"status": "ok", "name": str(model, "name"), "provider": str(provider, "provider"), "endpoint": str(provider, "endpoint"), "model": str(model, "model"), "models_status": responseStatus, "model_available": available, "slots_probe_skipped": true, "slots_source": "config", "slots": number(model, "max_parallel"), "context_window": number(model, "context_window"), "execution_tested": false}
	if str(provider, "auth_type") == "none" {
		slotProvider := copyObject(provider)
		endpoint := str(slotProvider, "endpoint")
		for _, suffix := range []string{"/chat/completions", "/messages", "/v1/", "/v1"} {
			endpoint = strings.TrimSuffix(endpoint, suffix)
		}
		slotProvider["endpoint"] = endpoint
		reply, err := m.request(ctx, slotProvider, "GET", "/slots", nil)
		result["slots_probe_skipped"] = false
		result["slots_source"] = "probe"
		var payload any
		if err == nil && reply.Status == 200 && json.Unmarshal(reply.Body, &payload) == nil {
			slots, window := parseSlots(payload)
			result["slots"] = slots
			if window > 0 {
				result["context_window"] = window
			}
		} else {
			result["slot_probe"] = "slot discovery unavailable"
		}
	}
	if opts["no-run"] == "true" {
		return result, nil
	}
	result["execution_tested"] = true
	result["execution_ok"] = false
	payload := object{"model": str(model, "model"), "messages": []object{{"role": "user", "content": "Reply with exactly: ok"}}, "max_tokens": 16}
	suffix := "/chat/completions"
	if str(provider, "provider") == "anthropic" {
		suffix = "/messages"
	}
	body, _ := json.Marshal(payload)
	execution, runErr := m.request(ctx, provider, "POST", suffix, body)
	if runErr != nil {
		result["execution_error"] = runErr.Error()
	} else if execution.Status < 200 || execution.Status >= 300 {
		result["execution_error"] = fmt.Sprintf("provider returned HTTP %d", execution.Status)
	} else {
		var reply object
		if json.Unmarshal(execution.Body, &reply) == nil {
			valid := false
			for _, choice := range rows(reply, "choices") {
				if msg, ok := choice["message"].(map[string]any); ok && str(msg, "content") != "" {
					valid = true
				}
			}
			for _, part := range rows(reply, "content") {
				if str(part, "text") != "" {
					valid = true
				}
			}
			result["execution_ok"] = valid
		}
	}
	return result, nil
}

func (m *Manager) discover(ctx context.Context, provider object) (object, error) {
	p := copyObject(provider)
	suffix := "/models"
	if target := str(p, "models_url"); target != "" {
		u, err := url.Parse(target)
		if err != nil || u.Host == "" {
			return nil, errors.New("invalid model discovery URL")
		}
		p["endpoint"] = u.Scheme + "://" + u.Host
		suffix = u.RequestURI()
	}
	ids := []string{}
	details := []object{}
	seen := map[string]bool{}
	last := ""
	for page := 0; page < 100; page++ {
		path := suffix
		if last != "" {
			separator := "?"
			if strings.Contains(path, "?") {
				separator = "&"
			}
			path += separator + "after_id=" + url.QueryEscape(last)
		}
		response, err := m.request(ctx, p, "GET", path, nil)
		if err != nil {
			return nil, fmt.Errorf("provider could not list models: %w; enter a model ID manually", err)
		}
		if response.Status < 200 || response.Status >= 300 {
			return nil, fmt.Errorf("provider could not list models (HTTP %d); enter a model ID manually", response.Status)
		}
		var root object
		if json.Unmarshal(response.Body, &root) != nil {
			return nil, errors.New("provider returned an invalid model list; enter a model ID manually")
		}
		listed, ok := root["data"].([]any)
		if !ok {
			listed, ok = root["models"].([]any)
		}
		if !ok {
			return nil, errors.New("provider returned an invalid model list; enter a model ID manually")
		}
		for _, item := range listed {
			row, ok := item.(map[string]any)
			if !ok {
				if id, ok := item.(string); ok {
					row = object{"id": id}
				}
			}
			id := str(row, "id")
			if id == "" {
				id = str(row, "name")
			}
			if id == "" {
				return nil, errors.New("provider model list contains an empty ID")
			}
			if seen[id] {
				continue
			}
			seen[id] = true
			entry := object{"id": id}
			for _, field := range []string{"display_name", "context_window", "max_output", "deprecated"} {
				if v, ok := row[field]; ok {
					entry[field] = v
				}
			}
			for input, output := range map[string]string{"max_input_tokens": "context_window", "max_tokens": "max_output", "max_output_tokens": "max_output"} {
				if n := number(row, input); n > 0 && number(entry, output) == 0 {
					entry[output] = n
				}
			}
			details = append(details, entry)
			ids = append(ids, id)
		}
		if !boolean(root, "has_more", false) {
			return object{"status": "ok", "models": ids, "details": details, "count": len(ids)}, nil
		}
		next := str(root, "last_id")
		if next == "" || next == last {
			return nil, errors.New("provider pagination did not advance")
		}
		last = next
	}
	return nil, errors.New("provider model list exceeded pagination limit")
}

func endpointHost(endpoint string) string {
	if strings.HasPrefix(endpoint, "//") {
		endpoint = "http:" + endpoint
	} else if !strings.Contains(endpoint, "://") {
		endpoint = "http://" + endpoint
	}
	u, err := url.Parse(endpoint)
	if err != nil {
		return ""
	}
	return strings.TrimSuffix(strings.ToLower(u.Hostname()), ".")
}
