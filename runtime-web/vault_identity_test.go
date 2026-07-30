package main

import (
	"os"
	"path/filepath"
	"testing"
)

func TestVaultAccountsAuthenticateAndOverrideFirstBoot(t *testing.T) {
	vault := &fakeWebchatVault{snapshot: webchatVaultSnapshot{
		PrimaryUser: "operator", PrimaryPass: "first-boot-secret",
		ExtraUsers: "alice:alice-secret",
	}}
	accounts := newVaultAccounts(vault, filepath.Join(t.TempDir(), "bootstrap-replaced"))
	for _, tc := range []struct {
		user, password string
		want           bool
	}{
		{"operator", "first-boot-secret", true},
		{"alice", "alice-secret", true},
		{"operator", "wrong", false},
	} {
		got, err := accounts.Authenticate(tc.user, tc.password)
		if err != nil || got != tc.want {
			t.Fatalf("Authenticate(%q) = %v, %v; want %v", tc.user, got, err, tc.want)
		}
	}
	if err := accounts.UpdatePassword("operator", "first-boot-secret", "Vault replacement"); err != nil {
		t.Fatal(err)
	}
	oldOK, _ := accounts.Authenticate("operator", "first-boot-secret")
	newOK, _ := accounts.Authenticate("operator", "Vault replacement")
	if oldOK || !newOK {
		t.Fatalf("Vault override did not retire old password: old=%v new=%v", oldOK, newOK)
	}
}

func TestVaultReplacementMarkerRetiresEveryBootstrapLogin(t *testing.T) {
	dir := t.TempDir()
	marker := filepath.Join(dir, "bootstrap-replaced")
	vault := &fakeWebchatVault{snapshot: webchatVaultSnapshot{
		PrimaryUser: "operator", PrimaryPass: "old-secret",
		ExtraUsers:   "alice:alice-secret",
		AccountsJSON: `{"virant":"new-secret"}`,
	}}
	if err := os.WriteFile(marker, []byte("virant\n"), 0o600); err != nil {
		t.Fatal(err)
	}
	accounts := newVaultAccounts(vault, marker)
	for _, tc := range []struct {
		user, password string
		want           bool
	}{
		{"virant", "new-secret", true},
		{"operator", "old-secret", false},
		{"alice", "alice-secret", false},
	} {
		got, err := accounts.Authenticate(tc.user, tc.password)
		if err != nil || got != tc.want {
			t.Fatalf("Authenticate(%q) = %v, %v; want %v", tc.user, got, err, tc.want)
		}
	}
}

func TestWebchatVaultExportParserRejectsUnknownAndDuplicateRecords(t *testing.T) {
	if _, err := parseWebchatVaultExport("unknown\teA==\n"); err == nil {
		t.Fatal("unknown record accepted")
	}
	if _, err := parseWebchatVaultExport("user\tb3A=\nuser\tb3A=\n"); err == nil {
		t.Fatal("duplicate record accepted")
	}
}
