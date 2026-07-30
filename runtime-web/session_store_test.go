package main

import (
	"path/filepath"
	"strings"
	"testing"
	"time"
)

func TestSignedSessionStoresNoBearerCredential(t *testing.T) {
	db, err := openDB(filepath.Join(t.TempDir(), "webchat.db"))
	if err != nil {
		t.Fatal(err)
	}
	defer db.Close()
	if _, err := db.Exec(`INSERT INTO sessions(token,username,expires_at) VALUES(?,?,?)`,
		"legacy-plaintext-bearer", "legacy", time.Now().Add(time.Hour).UTC().Format(time.RFC3339)); err != nil {
		t.Fatal(err)
	}
	vault := &fakeWebchatVault{}
	store, err := newSignedSessionStore(db, vault, time.Hour)
	if err != nil {
		t.Fatal(err)
	}
	var legacyCount int
	if err := db.QueryRow(`SELECT COUNT(*) FROM sessions WHERE token='legacy-plaintext-bearer'`).Scan(&legacyCount); err != nil || legacyCount != 0 {
		t.Fatalf("legacy bearer survived migration: count=%d err=%v", legacyCount, err)
	}
	token, err := store.CreateSession("alice")
	if err != nil {
		t.Fatal(err)
	}
	id, _, ok := strings.Cut(token, ".")
	if !ok {
		t.Fatalf("session credential is not signed: %q", token)
	}
	var persisted string
	if err := db.QueryRow(`SELECT token FROM sessions WHERE username='alice'`).Scan(&persisted); err != nil {
		t.Fatal(err)
	}
	if persisted != id || persisted == token || strings.Contains(token, persisted+".") == false {
		t.Fatalf("persisted value can authenticate: persisted=%q token=%q", persisted, token)
	}
	if user, err := store.ValidateSession(token); err != nil || user != "alice" {
		t.Fatalf("valid signed session rejected: user=%q err=%v", user, err)
	}
	replacement := "0"
	if strings.HasSuffix(token, "0") {
		replacement = "1"
	}
	tampered := token[:len(token)-1] + replacement
	if _, err := store.ValidateSession(tampered); err == nil {
		t.Fatal("tampered session accepted")
	}
	if len(vault.snapshot.SessionHMAC) != 64 {
		t.Fatalf("HMAC key was not created in Vault: length=%d", len(vault.snapshot.SessionHMAC))
	}
}
