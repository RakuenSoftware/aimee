package observability

import (
	"context"
	"crypto/ecdsa"
	"crypto/elliptic"
	"crypto/rand"
	"crypto/tls"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"io"
	"math/big"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type testPKI struct {
	caFile         string
	serverCertFile string
	serverKeyFile  string
	clientCert     tls.Certificate
	rootCAs        *x509.CertPool
}

func TestMetricsServerMutualTLS(t *testing.T) {
	pki := makeTestPKI(t)
	server, err := StartMetricsServer(MetricsServerConfig{
		Endpoint:           "tcp://0.0.0.0:0",
		TLSCertificateFile: pki.serverCertFile,
		TLSKeyFile:         pki.serverKeyFile,
		TLSClientCAFile:    pki.caFile,
	}, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Shutdown(context.Background()) })
	_, port, err := net.SplitHostPort(server.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	endpoint := "https://127.0.0.1:" + port + "/metrics"

	withoutCertificate := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS12, RootCAs: pki.rootCAs,
	}}}
	if response, err := withoutCertificate.Get(endpoint); err == nil {
		_ = response.Body.Close()
		t.Fatal("mTLS listener accepted a client without a certificate")
	}

	withCertificate := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS12, RootCAs: pki.rootCAs,
		Certificates: []tls.Certificate{pki.clientCert},
	}}}
	response, err := withCertificate.Get(endpoint)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("mTLS status = %d, want 200", response.StatusCode)
	}
}

func TestMetricsServerRequiresBearerInAdditionToMutualTLS(t *testing.T) {
	pki := makeTestPKI(t)
	const token = "0123456789abcdef0123456789abcdef"
	tokenFile := filepath.Join(t.TempDir(), "metrics.token")
	if err := os.WriteFile(tokenFile, []byte(token), 0o600); err != nil {
		t.Fatal(err)
	}
	server, err := StartMetricsServer(MetricsServerConfig{
		Endpoint:           "tcp://0.0.0.0:0",
		TLSCertificateFile: pki.serverCertFile,
		TLSKeyFile:         pki.serverKeyFile,
		TLSClientCAFile:    pki.caFile,
		BearerTokenFile:    tokenFile,
	}, http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) {
		_, _ = io.WriteString(w, "ok")
	}))
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { _ = server.Shutdown(context.Background()) })
	_, port, err := net.SplitHostPort(server.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	client := &http.Client{Transport: &http.Transport{TLSClientConfig: &tls.Config{
		MinVersion: tls.VersionTLS12, RootCAs: pki.rootCAs,
		Certificates: []tls.Certificate{pki.clientCert},
	}}}
	endpoint := "https://127.0.0.1:" + port + "/metrics"
	response, err := client.Get(endpoint)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusUnauthorized {
		t.Fatalf("mTLS-only status = %d, want 401", response.StatusCode)
	}

	request, err := http.NewRequest(http.MethodGet, endpoint, nil)
	if err != nil {
		t.Fatal(err)
	}
	request.Header.Set("Authorization", "Bearer "+token)
	response, err = client.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("mTLS + bearer status = %d, want 200", response.StatusCode)
	}
}

func makeTestPKI(t *testing.T) testPKI {
	t.Helper()
	directory := t.TempDir()
	now := time.Now()
	caKey := makeTestKey(t)
	caTemplate := &x509.Certificate{
		SerialNumber: big.NewInt(1), Subject: pkix.Name{CommonName: "Aimee Test CA"},
		NotBefore: now.Add(-time.Minute), NotAfter: now.Add(time.Hour), IsCA: true,
		BasicConstraintsValid: true, KeyUsage: x509.KeyUsageCertSign | x509.KeyUsageDigitalSignature,
	}
	caDER, err := x509.CreateCertificate(rand.Reader, caTemplate, caTemplate, &caKey.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	caCertificate, err := x509.ParseCertificate(caDER)
	if err != nil {
		t.Fatal(err)
	}
	caPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: caDER})
	caFile := filepath.Join(directory, "ca.pem")
	if err := os.WriteFile(caFile, caPEM, 0o644); err != nil {
		t.Fatal(err)
	}

	serverCertPEM, serverKeyPEM := issueTestCertificate(t, caCertificate, caKey, 2,
		"aimee-metrics", []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth}, []net.IP{net.ParseIP("127.0.0.1")})
	serverCertFile := filepath.Join(directory, "server.pem")
	serverKeyFile := filepath.Join(directory, "server.key")
	if err := os.WriteFile(serverCertFile, serverCertPEM, 0o644); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(serverKeyFile, serverKeyPEM, 0o600); err != nil {
		t.Fatal(err)
	}

	clientCertPEM, clientKeyPEM := issueTestCertificate(t, caCertificate, caKey, 3,
		"prometheus", []x509.ExtKeyUsage{x509.ExtKeyUsageClientAuth}, nil)
	clientCertificate, err := tls.X509KeyPair(clientCertPEM, clientKeyPEM)
	if err != nil {
		t.Fatal(err)
	}
	roots := x509.NewCertPool()
	if !roots.AppendCertsFromPEM(caPEM) {
		t.Fatal("append test CA")
	}
	return testPKI{
		caFile: caFile, serverCertFile: serverCertFile, serverKeyFile: serverKeyFile,
		clientCert: clientCertificate, rootCAs: roots,
	}
}

func issueTestCertificate(t *testing.T, ca *x509.Certificate, caKey *ecdsa.PrivateKey,
	serial int64, commonName string, usages []x509.ExtKeyUsage, ips []net.IP) ([]byte, []byte) {
	t.Helper()
	key := makeTestKey(t)
	template := &x509.Certificate{
		SerialNumber: big.NewInt(serial), Subject: pkix.Name{CommonName: commonName},
		NotBefore: time.Now().Add(-time.Minute), NotAfter: time.Now().Add(time.Hour),
		KeyUsage: x509.KeyUsageDigitalSignature, ExtKeyUsage: usages, IPAddresses: ips,
	}
	der, err := x509.CreateCertificate(rand.Reader, template, ca, &key.PublicKey, caKey)
	if err != nil {
		t.Fatal(err)
	}
	keyDER, err := x509.MarshalPKCS8PrivateKey(key)
	if err != nil {
		t.Fatal(err)
	}
	return pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der}),
		pem.EncodeToMemory(&pem.Block{Type: "PRIVATE KEY", Bytes: keyDER})
}

func makeTestKey(t *testing.T) *ecdsa.PrivateKey {
	t.Helper()
	key, err := ecdsa.GenerateKey(elliptic.P256(), rand.Reader)
	if err != nil {
		t.Fatal(err)
	}
	return key
}
