package egress

import (
	"context"
	"encoding/json"
	"github.com/JBailes/aimee/server-go/bus"
	"net"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"
)

type providerKeyCaller struct{ broker *credentialBroker }

func (c providerKeyCaller) Call(_ context.Context, event, stage uint32, _ uint64, _ time.Duration, _ []byte) ([]byte, error) {
	if event != EventCredentialKey || stage != StageCredentialKey {
		panic("unexpected event")
	}
	reply, _ := c.broker.publicReply()
	return reply, nil
}
func TestProviderCredentialBindsAccountOriginPortAndCaller(t *testing.T) {
	broker, err := newCredentialBroker()
	if err != nil {
		t.Fatal(err)
	}
	client, _ := NewBusAuthorizer(providerKeyCaller{broker})
	envelope, err := client.SealProviderCredential(context.Background(), 0, "http://127.0.0.1:18765/v1/models", "account-a", "bearer", []byte("fixture-secret"))
	if err != nil {
		t.Fatal(err)
	}
	request := HTTPRequest{CredentialHandle: "provider", CredentialScope: "bearer", CredentialResource: "account-a", Credential: envelope}
	inv := bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: ProvidersClientRef}
	now := time.Now()
	plaintext, err := broker.decrypt(now, inv, request, "http://127.0.0.1:18765")
	if err != nil || string(plaintext) != "fixture-secret" {
		t.Fatal(err)
	}
	clear(plaintext)
	for name, mutate := range map[string]func(*HTTPRequest, *bus.ModuleInvocation, *string){
		"account": func(r *HTTPRequest, _ *bus.ModuleInvocation, _ *string) { r.CredentialResource = "account-b" },
		"caller":  func(_ *HTTPRequest, i *bus.ModuleInvocation, _ *string) { i.PrincipalRef++ },
		"port":    func(_ *HTTPRequest, _ *bus.ModuleInvocation, h *string) { *h = "http://127.0.0.1:18766" },
		"scheme":  func(_ *HTTPRequest, _ *bus.ModuleInvocation, h *string) { *h = "https://127.0.0.1:18765" },
		"auth":    func(r *HTTPRequest, _ *bus.ModuleInvocation, _ *string) { r.CredentialScope = "x-api-key" },
		"tamper": func(r *HTTPRequest, _ *bus.ModuleInvocation, _ *string) {
			r.Credential.Ciphertext = strings.Repeat("A", len(r.Credential.Ciphertext))
		},
	} {
		t.Run(name, func(t *testing.T) {
			r := request
			e := *envelope
			r.Credential = &e
			i := inv
			host := "http://127.0.0.1:18765"
			mutate(&r, &i, &host)
			if p, err := broker.decrypt(now, i, r, host); err == nil {
				clear(p)
				t.Fatal("scope change accepted")
			}
		})
	}
	if p, err := broker.decrypt(now.Add(time.Minute), inv, request, "http://127.0.0.1:18765"); err == nil {
		clear(p)
		t.Fatal("expired key accepted")
	}
}

func TestCodexProbeHeadersAndSealedTokenReachProvider(t *testing.T) {
	upstream := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path != "/codex/responses" || r.Header.Get("Authorization") != "Bearer fixture-token" || r.Header.Get("ChatGPT-Account-ID") != "account-a" || r.Header.Get("originator") != "codex_cli_rs" {
			t.Error("provider did not receive its OAuth request")
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.Header().Set("Content-Type", "text/event-stream")
		_, _ = w.Write([]byte("data: {\"type\":\"response.completed\"}\n\n"))
	}))
	defer upstream.Close()
	broker, err := newCredentialBroker()
	if err != nil {
		t.Fatal(err)
	}
	client, _ := NewBusAuthorizer(providerKeyCaller{broker})
	target := upstream.URL + "/codex/responses"
	envelope, err := client.SealProviderCredential(context.Background(), 0, target, "codex", "bearer", []byte("fixture-token"))
	if err != nil {
		t.Fatal(err)
	}
	body := []byte(`{"model":"fixture","stream":true,"store":false}`)
	request := HTTPRequest{Request: Request{TargetURL: target, Purpose: "provider", Method: "POST", CredentialPresent: true, RequestSHA256: RequestDigest("POST", target, body, true)},
		Headers: map[string]string{"ChatGPT-Account-ID": "account-a", "originator": "codex_cli_rs", "Accept": "text/event-stream"},
		Body:    body, CredentialHandle: "provider", CredentialScope: "bearer", CredentialResource: "codex", Credential: envelope, MaxResponseBytes: 4096, TimeoutMS: 1000}
	wire, _ := json.Marshal(request)
	p := policy{resolver: fixedResolver{{IP: net.ParseIP("127.0.0.1")}}, credentials: broker}
	encoded, status := p.handleHTTP(bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: ProvidersClientRef, StageID: StageHTTP}, wire)
	response, err := decodeHTTPResponse(encoded)
	if status != bus.ModuleStatusOK || err != nil || response.Status != 200 || !strings.Contains(string(response.Body), "response.completed") {
		t.Fatal(status, response, err)
	}
}
