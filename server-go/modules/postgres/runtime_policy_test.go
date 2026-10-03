package postgres

import (
	"crypto/tls"
	"testing"

	"github.com/jackc/pgx/v5/pgconn"
	"github.com/jackc/pgx/v5/pgxpool"
)

func TestRuntimeTLSRequiresVerifiedHostAndEveryFallback(t *testing.T) {
	t.Setenv("AIMEE_KB_HARDENED", "1")
	for _, mode := range []string{"disable", "allow", "prefer", "require", "verify-ca"} {
		cfg, err := pgxpool.ParseConfig("host=localhost dbname=postgres sslmode=" + mode)
		if err != nil {
			t.Fatal("parse test configuration")
		}
		if validateRuntimeTLS(cfg) == nil {
			t.Fatalf("accepted %s", mode)
		}
	}
	cfg, err := pgxpool.ParseConfig("host=localhost dbname=postgres sslmode=verify-full")
	if err != nil {
		t.Fatal("parse verified test configuration")
	}
	if err := validateRuntimeTLS(cfg); err != nil {
		t.Fatal(err)
	}
	cfg.ConnConfig.Fallbacks = append(cfg.ConnConfig.Fallbacks, &pgconn.FallbackConfig{TLSConfig: &tls.Config{InsecureSkipVerify: true}})
	if validateRuntimeTLS(cfg) == nil {
		t.Fatal("insecure fallback accepted")
	}
	t.Setenv("AIMEE_KB_HARDENED", "0")
	if err := validateRuntimeTLS(cfg); err != nil {
		t.Fatal("changed development policy")
	}
}

func TestRuntimeTLSConninfoForms(t *testing.T) {
	t.Setenv("AIMEE_KB_HARDENED", "1")
	for _, dsn := range []string{"postgres://u:p@h:5432/db?sslmode=verify-full", "host=h dbname=db sslmode=verify-full", "host=h sslmode='verify-full'", "postgres://u:p@h/db?sslmode=verify-full&connect_timeout=5"} {
		cfg, err := parseStoreConfig(dsn)
		if err != nil || validateRuntimeTLS(cfg) != nil {
			t.Error("verified test DSN refused")
		}
	}
	for _, dsn := range []string{"postgres://u:p@h/db", "host=h password='sslmode=verify-full'", "host=h sslmode=verify-full sslmode=require", "postgres://u:p@h/db?sslmode=require", ""} {
		cfg, err := parseStoreConfig(dsn)
		if err == nil && validateRuntimeTLS(cfg) == nil {
			t.Error("unsafe test DSN accepted")
		}
	}
}
