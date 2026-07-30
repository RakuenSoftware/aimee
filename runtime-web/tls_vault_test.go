package main

import (
	"os"
	"path/filepath"
	"strings"
	"testing"
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
}
