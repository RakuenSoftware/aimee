package postgres

import (
	"context"
	"testing"
)

func TestOperatorConnectionPolicy(t *testing.T) {
	t.Setenv("PGHOST", "")
	for _, dsn := range []string{
		"host=/run/postgresql dbname=aimee",
		"host=192.0.2.10 sslmode=verify-full dbname=aimee",
		"host=db.example.test hostaddr=192.0.2.10 sslmode=verify-full dbname=aimee",
	} {
		cfg, err := operatorConfig(dsn)
		if err != nil {
			t.Errorf("allowed test case refused: %v", err)
			continue
		}
		if cfg.ConnConfig.Host == "db.example.test" {
			ips, err := cfg.ConnConfig.LookupFunc(context.Background(), "db.example.test")
			if err != nil || len(ips) != 1 || ips[0] != "192.0.2.10" {
				t.Fatal("DNS pin lost")
			}
			if cfg.ConnConfig.TLSConfig.ServerName != "db.example.test" {
				t.Fatal("certificate identity lost")
			}
		}
	}
	for _, dsn := range []string{
		"host=db.example.test sslmode=verify-full dbname=aimee",
		"host=192.0.2.10 sslmode=require dbname=aimee",
		"host=192.0.2.10,192.0.2.11 sslmode=verify-full dbname=aimee",
		"host=/run/postgresql hostaddr=127.0.0.1 dbname=aimee",
		"hostaddr=127.0.0.1 sslmode=verify-full dbname=aimee",
		"host=db.example.test hostaddr=address.example sslmode=verify-full dbname=aimee",
		"password='host=fake' hostaddr=127.0.0.1 sslmode=verify-full",
	} {
		if _, err := operatorConfig(dsn); err == nil {
			t.Error("unsafe test case accepted")
		}
	}
}
func TestExplicitHostCannotBeForgedInsideValues(t *testing.T) {
	for _, dsn := range []string{"password=' host=fake '", "password=host=fake", "password=foo\\ host=fake"} {
		if explicitHost(dsn) {
			t.Fatal("forged host")
		}
	}
	for _, dsn := range []string{"host = 'db.example'", "password='host=fake' host=real", "postgres://user@db.example/aimee", "postgres:///aimee?host=%2Frun%2Fpostgresql"} {
		if !explicitHost(dsn) {
			t.Fatal("host missed")
		}
	}
}
