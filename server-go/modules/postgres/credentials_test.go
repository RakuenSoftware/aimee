package postgres

import (
	"context"
	"testing"
)

func TestStoreCredentialBoundary(t *testing.T) {
	t.Setenv("AIMEE_HOME", "")
	t.Setenv("AIMEE_STORE_URL", "host=fixture user=runtime password=fixture-runtime")
	t.Setenv("AIMEE_STORE_MIGRATION_URL", "host=fixture user=migrator password=fixture-migrator")
	runtime, err := storeDSN(context.Background(), "AIMEE_STORE_URL")
	if err != nil || runtime != "host=fixture user=runtime password=fixture-runtime" {
		t.Fatal("native runtime credential unavailable")
	}
	migration, err := storeDSN(context.Background(), "AIMEE_STORE_MIGRATION_URL")
	if err != nil || migration == runtime {
		t.Fatal("migration authority was not separate")
	}
	for _, name := range []string{"AIMEE_LUKS_KEY", "AIMEE_KB_API_BEARER_TOKEN", "", "AIMEE_STORE_URL\x00"} {
		t.Setenv("AIMEE_LUKS_KEY", "must-not-read")
		if value, err := storeDSN(context.Background(), name); err == nil || value != "" {
			t.Fatal("unregistered credential accepted")
		}
	}
	t.Setenv("AIMEE_STORE_URL", "")
	if value, err := storeDSN(context.Background(), "AIMEE_STORE_URL"); err == nil || value != "" {
		t.Fatal("missing authority must fail closed")
	}
}
