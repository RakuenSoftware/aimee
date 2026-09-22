package providers

import (
	"context"
	"encoding/json"
	"errors"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"strings"
	"testing"
)

type fixtureNetwork struct {
	requests []egress.HTTPRequest
	secrets  []string
	replies  []egress.HTTPResponse
	err      error
}

func (f *fixtureNetwork) Do(_ context.Context, _ uint64, req egress.HTTPRequest) (egress.HTTPResponse, error) {
	f.requests = append(f.requests, req)
	if f.err != nil {
		return egress.HTTPResponse{}, f.err
	}
	r := f.replies[0]
	f.replies = f.replies[1:]
	return r, nil
}
func (f *fixtureNetwork) SealProviderCredential(_ context.Context, _ uint64, target, account, auth string, secret []byte) (*egress.CredentialEnvelope, error) {
	f.secrets = append(f.secrets, string(secret))
	return &egress.CredentialEnvelope{Resource: account, Host: target, Operation: auth}, nil
}
func response(status int, body string) egress.HTTPResponse {
	return egress.HTTPResponse{Status: status, Body: []byte(body)}
}
func TestDiscoveryPaginationAndPublishedLimits(t *testing.T) {
	m, _, _ := manager(t)
	network := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, `{"data":[{"id":"one","max_input_tokens":1000000,"max_tokens":128000}],"has_more":true,"last_id":"one"}`), response(200, `{"data":[{"id":"two"}],"has_more":false}`)}}
	m.SetEgress(network)
	call(t, m, "provider.save_connection", connection("work", "key"))
	out := call(t, m, "provider.connection_models", object{"name": "work"})
	if len(rows(out, "details")) != 2 || number(rows(out, "details")[0], "context_window") != 1000000 {
		t.Fatal(out)
	}
	if _, ok := rows(out, "details")[1]["context_window"]; ok {
		t.Fatal("invented capacity")
	}
	if !strings.HasSuffix(network.requests[1].TargetURL, "?after_id=one") {
		t.Fatal(network.requests)
	}
	for _, request := range network.requests {
		if request.Headers["Authorization"] != "" || request.Credential == nil || request.Credential.Resource != "work" {
			t.Fatal("credential boundary violated")
		}
	}
}
func TestDiscoveryErrorsStayErrors(t *testing.T) {
	for _, reply := range []egress.HTTPResponse{response(401, `{}`), response(200, `bad json`), response(200, `{}`), response(200, `{"data":[],"has_more":true}`)} {
		m, _, _ := manager(t)
		m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{reply}})
		call(t, m, "provider.save_connection", connection("work", "key"))
		if _, err := m.Manage(context.Background(), Request{Operation: "provider.connection_models", Arguments: object{"name": "work"}}); err == nil {
			t.Fatal("discovery failure masked")
		}
	}
}
func TestProbeNoRunAndExecutionFailure(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "provider.save_connection", connection("work", "key"))
	call(t, m, "model.add", object{"args": []string{"model", "unused", "m", "--registration", "work"}})
	m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{response(200, `{"data":[{"id":"m"}]}`)}})
	out := call(t, m, "model.probe", object{"args": []string{"model", "--no-run"}})
	if _, exists := out["execution_ok"]; exists || boolean(out, "execution_tested", true) {
		t.Fatal(out)
	}
	m.SetEgress(&fixtureNetwork{err: errors.New("offline")})
	out = call(t, m, "model.probe", object{"args": []string{"model"}})
	if boolean(out, "execution_ok", true) || !boolean(out, "execution_tested", false) {
		t.Fatal(out)
	}
}

func TestBundledSynthesisProbeRequiresFinalText(t *testing.T) {
	for _, final := range []bool{false, true} {
		m, _, _ := manager(t)
		call(t, m, "model.add", object{"args": []string{"local", "https://aimee-llm:8761/v1", "m", "--auth-type", "none"}})
		body := `{"choices":[{"message":{"content":"","reasoning_content":"thinking"},"finish_reason":"length"}]}`
		if final {
			body = `{"choices":[{"message":{"content":"ok"},"finish_reason":"stop"}]}`
		}
		network := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, `{"data":[{"id":"m"}]}`), response(200, `[]`), response(200, body)}}
		m.SetEgress(network)
		out := call(t, m, "model.probe", object{"args": []string{"local"}})
		if boolean(out, "execution_ok", false) != final {
			t.Fatal(out)
		}
		if !final && str(out, "execution_error") == "" {
			t.Fatal("empty final text has no failure explanation")
		}
		var request object
		if json.Unmarshal(network.requests[len(network.requests)-1].Body, &request) != nil {
			t.Fatal("invalid request")
		}
		kwargs, _ := request["chat_template_kwargs"].(map[string]any)
		if boolean(kwargs, "enable_thinking", true) || number(request, "max_tokens") < 128 {
			t.Fatal("probe can exhaust its budget before producing final text")
		}
	}
}

func TestAnthropicProbeUsesVersionedMessagesRoute(t *testing.T) {
	for _, endpoint := range []string{"https://api.minimax.io/anthropic", "https://api.minimax.io/anthropic/", "https://api.minimax.io/anthropic/v1", "https://api.minimax.io/anthropic/v1/messages"} {
		t.Run(endpoint, func(t *testing.T) {
			m, _, _ := manager(t)
			p := connection("minimax", "fixture-key")
			p["provider"], p["endpoint"], p["auth_type"] = "anthropic", endpoint, "x-api-key"
			call(t, m, "provider.save_connection", p)
			call(t, m, "model.add", object{"args": []string{"mini", "unused", "MiniMax-M3", "--registration", "minimax"}})
			network := &fixtureNetwork{replies: []egress.HTTPResponse{response(404, `{}`), response(200, `{"content":[{"type":"text","text":"ok"}]}`)}}
			m.SetEgress(network)
			out := call(t, m, "model.probe", object{"args": []string{"mini"}})
			if !boolean(out, "execution_ok", false) {
				t.Fatal(out)
			}
			request := network.requests[1]
			if request.TargetURL != "https://api.minimax.io/anthropic/v1/messages" || request.Headers["anthropic-version"] != "2023-06-01" || request.CredentialScope != "x-api-key" {
				t.Fatalf("wrong Anthropic request: %+v", request)
			}
			if request.TimeoutMS < 30000 {
				t.Fatal("inference has the short discovery timeout")
			}
		})
	}
}

const codexCompleted = "event: response.completed\ndata: " + `{"type":"response.completed","response":{"status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}]}}` + "\n\n"

func TestCodexProbeUsesResponsesAndSelectedOAuthAccount(t *testing.T) {
	for _, document := range []bool{false, true} {
		m, _, vault := manager(t)
		p := connection("codex", "wrong-api-key")
		p["provider"], p["endpoint"], p["auth_type"] = "chatgpt", "https://chatgpt.com/backend-api/codex", "codex-oauth"
		// OAuth connections are created by login/legacy migration, not API-key CRUD.
		_, err := m.store.transaction(true, func(root object) (object, error) {
			delete(p, "api_key")
			root["providers"] = []object{p}
			root["models"] = []object{{"name": "codex:model", "registration": "codex", "model": "test-model"}}
			return nil, nil
		})
		if err != nil {
			t.Fatal(err)
		}
		vault.values["codex:api_key"] = "wrong-api-key"
		if document {
			vault.values["codex:oauth"] = `{"tokens":{"access_token":"fixture-oauth","account_id":"account-a"}}`
		} else {
			vault.values["codex:codex_oauth_token"] = "fixture-oauth"
			vault.values["codex:codex_account_id"] = "account-a"
		}
		network := &fixtureNetwork{replies: []egress.HTTPResponse{response(200, codexCompleted)}}
		m.SetEgress(network)
		out := call(t, m, "model.probe", object{"args": []string{"codex:model"}})
		if !boolean(out, "execution_ok", false) || !boolean(out, "model_available", false) || !boolean(out, "models_probe_skipped", false) || len(network.requests) != 1 {
			t.Fatal(out, len(network.requests))
		}
		request := network.requests[0]
		if request.TargetURL != "https://chatgpt.com/backend-api/codex/responses" || request.CredentialScope != "bearer" || request.Headers["ChatGPT-Account-ID"] != "account-a" || request.Headers["originator"] != "codex_cli_rs" {
			t.Fatalf("wrong Responses request: %+v", request)
		}
		if len(network.secrets) != 1 || network.secrets[0] != "fixture-oauth" {
			t.Fatal("OAuth token was not sealed")
		}
		var payload object
		if json.Unmarshal(request.Body, &payload) != nil || str(payload, "model") != "test-model" || !boolean(payload, "stream", false) || boolean(payload, "store", true) || str(payload, "instructions") == "" || len(rows(payload, "input")) != 1 {
			t.Fatal(string(request.Body))
		}
		for _, forbidden := range []string{"max_tokens", "max_output_tokens", "messages"} {
			if _, ok := payload[forbidden]; ok {
				t.Fatal("unsupported Codex field", forbidden)
			}
		}
		wire, _ := json.Marshal(request)
		if strings.Contains(string(wire), "fixture-oauth") || request.Headers["Authorization"] != "" {
			t.Fatal("plaintext credential on bus")
		}
		out = call(t, m, "model.probe", object{"args": []string{"codex:model", "--no-run"}})
		if len(network.requests) != 1 || boolean(out, "execution_tested", true) {
			t.Fatal("no-run executed inference")
		}
	}
}

func TestCodexProbeDoesNotBorrowAnotherConnectionsCredentials(t *testing.T) {
	m, _, vault := manager(t)
	vault.values["codex:codex_oauth_token"] = "other-account"
	vault.values["personal:api_key"] = "not-an-oauth-token"
	if _, _, err := m.resolveCodex(context.Background(), object{"name": "personal"}); err == nil {
		t.Fatal("missing selected OAuth credential was accepted")
	}
}

func TestProbeResponseRequiresFinalSuccessfulOutput(t *testing.T) {
	for _, tc := range []struct {
		name, body    string
		responses, ok bool
	}{
		{"codex", codexCompleted, true, true},
		{"delta-only", "data: {\"type\":\"response.output_text.delta\",\"delta\":\"ok\"}\n\n", true, false},
		{"failed-after-text", codexCompleted + "data: {\"type\":\"response.failed\"}\n\n", true, false},
		{"incomplete", strings.ReplaceAll(codexCompleted, "completed", "incomplete"), true, false},
		{"reasoning-only", strings.ReplaceAll(codexCompleted, "output_text", "reasoning_text"), true, false},
		{"bad-event", "data: not json\n\n", true, false},
		{"openai", `{"choices":[{"message":{"content":"ok"}}]}`, false, true},
		{"anthropic", `{"content":[{"type":"text","text":"ok"}]}`, false, true},
		{"invalid-json", "not json", false, false},
		{"empty", `{}`, false, false},
	} {
		t.Run(tc.name, func(t *testing.T) {
			if err := probeResponse([]byte(tc.body), tc.responses); (err == nil) != tc.ok {
				t.Fatal(err)
			}
		})
	}
}

func TestCodexProbeAcceptsStreamedOutputWithEmptyCompletion(t *testing.T) {
	completed := "data: " + `{"type":"response.completed","response":{"status":"completed","output":[]}}` + "\n\n"
	for _, output := range []string{
		`{"type":"response.output_item.done","item":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"ok"}]}}`,
		`{"type":"response.output_text.done","text":"ok"}`,
		`{"type":"response.content_part.done","part":{"type":"output_text","text":"ok"}}`,
	} {
		stream := "data: " + output + "\n\n"
		if err := probeResponse([]byte(stream+completed), true); err != nil {
			t.Fatal(err)
		}
		if err := probeResponse([]byte(stream), true); err == nil {
			t.Fatal("incomplete stream accepted")
		}
		if err := probeResponse([]byte(stream+"data: {\"type\":\"response.failed\"}\n\n"), true); err == nil {
			t.Fatal("failed stream accepted")
		}
	}
}

func TestProbeFailureRetainsNativeClientFields(t *testing.T) {
	m, _, _ := manager(t)
	call(t, m, "provider.save_connection", connection("work", "key"))
	call(t, m, "model.add", object{"args": []string{"model", "unused", "m", "--registration", "work"}})
	m.SetEgress(&fixtureNetwork{replies: []egress.HTTPResponse{response(200, `{"data":[{"id":"m"}]}`), response(403, `{}`)}})
	out := call(t, m, "model.probe", object{"args": []string{"model"}})
	if str(out, "execution_message") != "provider returned HTTP 403" || out["detected_slots"] != out["slots"] || out["detected_context_window"] != out["context_window"] {
		t.Fatal(out)
	}
}
