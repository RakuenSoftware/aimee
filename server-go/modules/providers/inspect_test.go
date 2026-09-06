package providers

import (
	"context"
	"errors"
	"github.com/JBailes/aimee/server-go/modules/egress"
	"strings"
	"testing"
)

type fixtureNetwork struct {
	requests []egress.HTTPRequest
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
func (f *fixtureNetwork) SealProviderCredential(_ context.Context, _ uint64, target, account, auth string, _ []byte) (*egress.CredentialEnvelope, error) {
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
