package main

import (
	"crypto/rand"
	"crypto/x509"
	"crypto/x509/pkix"
	"encoding/pem"
	"math/big"
	"os"
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestTLSPrivateKeyExistsOnlyInVault(t *testing.T) {
	dir := t.TempDir()
	cfg := &config{port: 8443, dbPath: filepath.Join(dir, "webchat.db")}
	vault := &fakeWebchatVault{}
	first, err := ensureTLSCertificate(cfg, vault)
	if err != nil {
		t.Fatal(err)
	}
	if len(first.Certificate) == 0 || !strings.Contains(vault.snapshot.TLSKey, "PRIVATE KEY") {
		t.Fatal("TLS identity was not generated with its key in Vault")
	}
	if _, err := os.Stat(filepath.Join(dir, "webchat.key")); !os.IsNotExist(err) {
		t.Fatalf("TLS private-key file exists: %v", err)
	}
	if _, err := os.Stat(filepath.Join(dir, "webchat.crt")); err != nil {
		t.Fatalf("public certificate missing: %v", err)
	}
	second, err := ensureTLSCertificate(cfg, vault)
	if err != nil || len(second.Certificate) == 0 {
		t.Fatalf("Vault TLS identity did not survive restart: %v", err)
	}
	if string(first.Certificate[0]) != string(second.Certificate[0]) {
		t.Fatal("a certificate that covers the current address set was needlessly rotated")
	}
}

func TestLegacyTLSCertificateIsRotatedForCurrentAddresses(t *testing.T) {
	dir := t.TempDir()
	cfg := &config{port: 8443, dbPath: filepath.Join(dir, "webchat.db")}
	vault := &fakeWebchatVault{}
	if _, err := ensureTLSCertificate(cfg, vault); err != nil {
		t.Fatal(err)
	}
	keyPEM := []byte(vault.snapshot.TLSKey)
	signer, err := parseTLSPrivateKey(keyPEM)
	if err != nil {
		t.Fatal(err)
	}
	legacy := &x509.Certificate{
		SerialNumber: big.NewInt(1),
		Subject:      pkix.Name{CommonName: "aimee-runtime-web"},
		NotBefore:    time.Now().Add(-time.Minute),
		NotAfter:     time.Now().Add(time.Hour),
		KeyUsage:     x509.KeyUsageDigitalSignature,
		ExtKeyUsage:  []x509.ExtKeyUsage{x509.ExtKeyUsageServerAuth},
		DNSNames:     []string{"legacy.invalid"},
	}
	der, err := x509.CreateCertificate(rand.Reader, legacy, legacy, signer.Public(), signer)
	if err != nil {
		t.Fatal(err)
	}
	legacyPEM := pem.EncodeToMemory(&pem.Block{Type: "CERTIFICATE", Bytes: der})
	certPath := filepath.Join(dir, "webchat.crt")
	if err := os.WriteFile(certPath, legacyPEM, 0o644); err != nil {
		t.Fatal(err)
	}

	rotated, err := ensureTLSCertificate(cfg, vault)
	if err != nil {
		t.Fatal(err)
	}
	if string(rotated.Certificate[0]) == string(der) {
		t.Fatal("legacy certificate without current SANs was reused")
	}
	currentPEM, err := os.ReadFile(certPath)
	if err != nil || !certificateCoversCurrentSANs(currentPEM) {
		t.Fatalf("rotated certificate does not cover the current address set: %v", err)
	}
}
