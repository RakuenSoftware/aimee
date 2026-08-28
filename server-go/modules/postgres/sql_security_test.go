package postgres

import (
	"strings"
	"testing"
	"time"
)

func TestParseStoreConfigRejectsInvalidUTF8WithoutLooping(t *testing.T) {
	done := make(chan error, 1)
	go func() {
		_, err := parseStoreConfig("postgres://user:pass@localhost/db?service=" + string([]byte{0xff, 0xfe}))
		done <- err
	}()

	select {
	case err := <-done:
		if err == nil {
			t.Fatal("invalid UTF-8 in AIMEE_STORE_URL was accepted")
		}
		if !strings.Contains(err.Error(), "AIMEE_STORE_URL") {
			t.Fatalf("error did not identify the input boundary: %v", err)
		}
	case <-time.After(time.Second):
		t.Fatal("invalid UTF-8 made DSN parsing fail to terminate")
	}
}

func TestParseMigrationConfigRequiresDifferentDatabaseRole(t *testing.T) {
	_, err := parseMigrationConfig(
		"postgres://shared:one@localhost/db?application_name=migrator",
		"postgres://shared:one@localhost/db?application_name=runtime")
	if err == nil || !strings.Contains(err.Error(), "must differ") {
		t.Fatalf("same database role was not rejected: %v", err)
	}

	config, err := parseMigrationConfig("postgres://migrator:one@localhost/db",
		"postgres://runtime:two@localhost/db")
	if err != nil {
		t.Fatalf("distinct roles were rejected: %v", err)
	}
	if config.ConnConfig.User != "migrator" {
		t.Fatalf("migration role = %q, want migrator", config.ConnConfig.User)
	}
}
