package egress

import (
	"crypto/aes"
	"crypto/cipher"
	"crypto/ecdh"
	"crypto/rand"
	"crypto/sha256"
	"encoding/base64"
	"encoding/hex"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"strings"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
)

func sealCredentialForTest(t *testing.T, broker *credentialBroker, plaintext string,
	expires time.Time) CredentialEnvelope {
	t.Helper()
	envelope := CredentialEnvelope{Version: credentialVersion, KeyID: broker.keyID,
		ExpiresAt: expires.Unix(), Handle: credentialHandleForge, Host: credentialHostForge,
		Operation: "pr_info", Resource: "acme/widgets", PrincipalRef: GitClientRef}
	sender, err := ecdh.X25519().GenerateKey(rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	shared, err := sender.ECDH(broker.private.PublicKey())
	if err != nil {
		t.Fatal(err)
	}
	keyInput := append([]byte(nil), credentialKDFDomain...)
	keyInput = append(keyInput, shared...)
	keyInput = append(keyInput, sender.PublicKey().Bytes()...)
	keyInput = append(keyInput, broker.private.PublicKey().Bytes()...)
	key := sha256.Sum256(keyInput)
	block, err := aes.NewCipher(key[:])
	if err != nil {
		t.Fatal(err)
	}
	gcm, err := cipher.NewGCM(block)
	if err != nil {
		t.Fatal(err)
	}
	nonce := make([]byte, gcm.NonceSize())
	if _, err := rand.Read(nonce); err != nil {
		t.Fatal(err)
	}
	aad, err := credentialAAD(envelope)
	if err != nil {
		t.Fatal(err)
	}
	envelope.EphemeralPublicKey = base64.StdEncoding.EncodeToString(sender.PublicKey().Bytes())
	envelope.Nonce = base64.StdEncoding.EncodeToString(nonce)
	envelope.Ciphertext = base64.StdEncoding.EncodeToString(gcm.Seal(nil, nonce, []byte(plaintext), aad))
	return envelope
}

func TestCredentialEnvelopeIsCallerScopeAndExpiryBound(t *testing.T) {
	broker, err := newCredentialBroker()
	if err != nil {
		t.Fatal(err)
	}
	now := time.Now().Truncate(time.Second)
	envelope := sealCredentialForTest(t, broker, "secret-bearer", now.Add(30*time.Second))
	request := HTTPRequest{CredentialHandle: credentialHandleForge, CredentialScope: "pr_info",
		CredentialResource: "acme/widgets", Credential: &envelope}
	invocation := bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: GitClientRef}
	plaintext, err := broker.decrypt(now, invocation, request, credentialHostForge)
	if err != nil || string(plaintext) != "secret-bearer" {
		t.Fatalf("decrypt = %q, %v", plaintext, err)
	}
	clear(plaintext)

	for name, mutate := range map[string]func(*HTTPRequest, *bus.ModuleInvocation){
		"caller":     func(_ *HTTPRequest, invocation *bus.ModuleInvocation) { invocation.PrincipalRef++ },
		"operation":  func(request *HTTPRequest, _ *bus.ModuleInvocation) { request.CredentialScope = "pr_edit" },
		"repository": func(request *HTTPRequest, _ *bus.ModuleInvocation) { request.CredentialResource = "acme/other" },
		"key": func(request *HTTPRequest, _ *bus.ModuleInvocation) {
			request.Credential.KeyID = strings.Repeat("0", 32)
		},
	} {
		t.Run(name, func(t *testing.T) {
			copyEnvelope := envelope
			copyRequest := request
			copyRequest.Credential = &copyEnvelope
			copyInvocation := invocation
			mutate(&copyRequest, &copyInvocation)
			if plaintext, err := broker.decrypt(now, copyInvocation, copyRequest, credentialHostForge); err == nil {
				clear(plaintext)
				t.Fatal("mutated credential envelope was accepted")
			}
		})
	}
	if plaintext, err := broker.decrypt(now.Add(31*time.Second), invocation, request, credentialHostForge); err == nil {
		clear(plaintext)
		t.Fatal("expired credential envelope was accepted")
	}
}

func TestDecryptsEnvelopeProducedByTheCCore(t *testing.T) {
	// Produced by src/egress_credential_envelope.c with the RFC 7748
	// Alice private/public pair below. This fixture pins the cross-language byte
	// grammar (KDF order, AAD terminators, integer endian and GCM tag layout).
	privateBytes, err := hex.DecodeString("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
	if err != nil {
		t.Fatal(err)
	}
	private, err := ecdh.X25519().NewPrivateKey(privateBytes)
	if err != nil {
		t.Fatal(err)
	}
	broker := &credentialBroker{private: private, keyID: "0123456789abcdef0123456789abcdef"}
	envelope := CredentialEnvelope{Version: 1, KeyID: broker.keyID,
		EphemeralPublicKey: "Pqx6PPXElwuvJcJ4sKbUWbB63QcNzfPajds8Og5K9Xo=",
		Nonce:              "VK1RUYhS7OWfex+p", Ciphertext: "zAfd4wbEWZvmepVw/bPHFUqzOeOyqrlEqEU=",
		ExpiresAt: 1787838120, Handle: "forge", Host: "api.github.com", Operation: "pr_merge",
		Resource: "acme/widgets", PrincipalRef: GitClientRef}
	request := HTTPRequest{CredentialHandle: "forge", CredentialScope: "pr_merge",
		CredentialResource: "acme/widgets", Credential: &envelope}
	plaintext, err := broker.decrypt(time.Unix(envelope.ExpiresAt-1, 0),
		bus.ModuleInvocation{PrincipalClass: 1, PrincipalRef: GitClientRef}, request, "api.github.com")
	if err != nil || string(plaintext) != "test-token" {
		t.Fatalf("C/Go credential envelope interop = %q, %v", plaintext, err)
	}
	clear(plaintext)
}

func TestCredentialPublicKeyStageContainsNoPrivateMaterial(t *testing.T) {
	handler := newHandler(fixedResolver{})
	body, status := handler(bus.ModuleInvocation{PrincipalClass: 1, StageID: StageCredentialKey}, nil)
	if status != bus.ModuleStatusOK {
		t.Fatalf("status = %v", status)
	}
	var reply struct {
		Version   int    `json:"version"`
		KeyID     string `json:"key_id"`
		PublicKey string `json:"public_key"`
	}
	if json.Unmarshal(body, &reply) != nil || reply.Version != credentialVersion ||
		len(reply.KeyID) != credentialKeyIDBytes*2 || len(reply.PublicKey) == 0 || strings.Contains(string(body), "private") {
		t.Fatalf("public-key reply = %s", body)
	}
	if _, status := handler(bus.ModuleInvocation{PrincipalClass: 1, StageID: StageCredentialKey}, []byte(`{}`)); status != bus.ModuleStatusInvalidRequest {
		t.Fatalf("non-empty key request status = %v", status)
	}
}

func TestHTTPExecutorAddsDecryptedBearerOnlyAtNetworkBoundary(t *testing.T) {
	seen := ""
	server := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, request *http.Request) {
		seen = request.Header.Get("Authorization")
		w.WriteHeader(http.StatusNoContent)
	}))
	defer server.Close()
	response := (policy{}).executeHTTP(bus.ModuleInvocation{}, HTTPRequest{Request: Request{
		TargetURL: server.URL, Method: http.MethodGet}, MaxResponseBytes: 16, TimeoutMS: 1000},
		Decision{Target: server.URL, ResolvedIPs: []string{"127.0.0.1"}}, []byte("boundary-secret"))
	if response.Error != "" || response.Status != http.StatusNoContent || seen != "Bearer boundary-secret" {
		t.Fatalf("response=%+v Authorization=%q", response, seen)
	}
}

func TestForgeCredentialScopeMatchesOnlyItsExactOperationAndRepository(t *testing.T) {
	if !forgeCredentialTargetAllowed("pr_info", "acme/widgets", "GET", "/repos/acme/widgets/pulls/7") {
		t.Fatal("valid scoped request was rejected")
	}
	for _, request := range []struct{ operation, resource, method, path string }{
		{"pr_edit", "acme/widgets", "GET", "/repos/acme/widgets/pulls/7"},
		{"pr_info", "acme/other", "GET", "/repos/acme/widgets/pulls/7"},
		{"pr_info", "acme/widgets", "GET", "/repos/acme/widgets/issues/7"},
		{"pr_info", "acme/widgets", "GET", "/repos/acme/widgets/pulls/7/merge"},
	} {
		if forgeCredentialTargetAllowed(request.operation, request.resource, request.method, request.path) {
			t.Fatalf("out-of-scope request accepted: %+v", request)
		}
	}
}
