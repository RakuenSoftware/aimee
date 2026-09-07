package egress

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"encoding/pem"
	"math/big"
	"net"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestModelTLSCannotFollowCallerSelectedOriginOrPurpose(t *testing.T) {
	root := t.TempDir()
	for _, test := range []struct {
		target, purpose string
		wantError       bool
	}{
		{"https://external.example:8762/embed", "embedding", false},
		{"http://aimee-embedder:8762/embed", "embedding", true},
		{"https://aimee-embedder:443/embed", "embedding", true},
		{"https://aimee-embedder:8762/embed", "provider", true},
		{"https://aimee-embedder:8762/embed", "embedding", true}, // missing identity fails closed
		{"https://aimee-llm:8761/v1", "embedding", true},
		{"https://aimee-llm:8761/v1", "provider", true},
	} {
		target, _ := url.Parse(test.target)
		cfg, err := modelTLSConfig(target, test.purpose, root)
		if (err != nil) != test.wantError {
			t.Errorf("%s/%s: %v", test.target, test.purpose, err)
		}
		if err == nil && (cfg.RootCAs != nil || len(cfg.Certificates) != 0) {
			t.Fatal("external target acquired local trust/identity")
		}
	}
}

func TestModelTLSRealHandshakeRequiresOwnerIdentity(t *testing.T) {
	now := time.Now()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	ca := &x509.Certificate{SerialNumber: big.NewInt(1), IsCA: true, BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign, NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour)}
	der, err := x509.CreateCertificate(rand.Reader, ca, ca, &key.PublicKey, key)
	if err != nil {
		t.Fatal(err)
	}
	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	issue := func(serial int64, client bool) (tls.Certificate, []byte, []byte) {
		t.Helper()
		leafKey, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
		if err != nil {
			t.Fatal(err)
		}
		leaf := &x509.Certificate{SerialNumber: big.NewInt(serial), NotBefore: ca.NotBefore, NotAfter: ca.NotAfter, KeyUsage: x509.KeyUsageDigitalSignature, DNSNames: []string{"aimee-llm"}, ExtKeyUsage: []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}}
		if client {
			leaf.ExtKeyUsage = []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}
		}
		der, err := x509.CreateCertificate(rand.Reader, leaf, ca, &leafKey.PublicKey, key)
		if err != nil {
			t.Fatal(err)
		}
		keyDER, err := x509.MarshalPKCS8PrivateKey(leafKey)
		if err != nil {
			t.Fatal(err)
		}
		certPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
		keyPEM := pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
		cert, err := tls.X509KeyPair(certPEM, keyPEM)
		if err != nil {
			t.Fatal(err)
		}
		return cert, certPEM, keyPEM
	}
	serverCert, _, _ := issue(2, false)
	_, clientPEM, keyPEM := issue(3, true)
	pool := x509.NewCertPool()
	pool.AppendCertsFromPEM(caPEM)
	server := httptest.NewUnstartedServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) { w.WriteHeader(http.StatusNoContent) }))
	server.TLS = &tls.Config{Certificates: []tls.Certificate{serverCert}, ClientAuth: tls.RequireAndVerifyClientCert, ClientCAs: pool, MinVersion: tls.VersionTLS12}
	server.StartTLS()
	defer server.Close()
	root := t.TempDir()
	dir := filepath.Join(root, "synthesis", "client")
	if err := os.MkdirAll(dir, 0700); err != nil {
		t.Fatal(err)
	}
	for name, data := range map[string][]byte{"ca.pem": caPEM, "client.pem": clientPEM, "client.key": keyPEM} {
		if err := os.WriteFile(filepath.Join(dir, name), data, 0600); err != nil {
			t.Fatal(err)
		}
	}
	target, _ := url.Parse("https://aimee-llm:8761/v1/chat/completions")
	cfg, err := modelTLSConfig(target, "provider", root)
	if err != nil {
		t.Fatal(err)
	}
	for _, anonymous := range []bool{false, true} {
		c := cfg.Clone()
		if anonymous {
			c.Certificates = nil
		}
		transport := &http.Transport{TLSClientConfig: c, DialContext: func(ctx context.Context, network, address string) (net.Conn, error) {
			return (&net.Dialer{}).DialContext(ctx, network, server.Listener.Addr().String())
		}}
		client := &http.Client{Transport: transport, Timeout: 5 * time.Second}
		response, err := client.Get(target.String())
		transport.CloseIdleConnections()
		if response != nil {
			response.Body.Close()
		}
		if anonymous && err == nil {
			t.Fatal("anonymous client accepted")
		}
		if !anonymous && (err != nil || response.StatusCode != http.StatusNoContent) {
			t.Fatalf("owner identity refused: %v", err)
		}
	}
}
