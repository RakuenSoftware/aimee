package postgres

import (
	"context"
	"errors"
	"net"
	"net/url"
	"strings"
	"time"

	"github.com/jackc/pgx/v5/pgxpool"
)

// explicitHost inspects presence only; pgx remains the sole parser of values.
// Quoted/escaped values cannot masquerade as a host keyword.
func explicitHost(dsn string) bool {
	if strings.HasPrefix(dsn, "postgres://") || strings.HasPrefix(dsn, "postgresql://") {
		u, err := url.Parse(dsn)
		return err == nil && (u.Hostname() != "" || u.Query().Has("host"))
	}
	for len(dsn) > 0 {
		dsn = strings.TrimLeft(dsn, " \t\r\n")
		at := strings.IndexByte(dsn, '=')
		if at < 0 {
			return false
		}
		key := strings.TrimSpace(dsn[:at])
		dsn = strings.TrimLeft(dsn[at+1:], " \t\r\n")
		if key == "host" {
			return true
		}
		quoted := len(dsn) > 0 && dsn[0] == '\''
		if quoted {
			dsn = dsn[1:]
		}
		for len(dsn) > 0 {
			c := dsn[0]
			dsn = dsn[1:]
			if c == '\\' && len(dsn) > 0 {
				dsn = dsn[1:]
				continue
			}
			if (quoted && c == '\'') || (!quoted && strings.ContainsRune(" \t\r\n", rune(c))) {
				break
			}
		}
	}
	return false
}
func dnsIdentity(host string) bool {
	if len(host) == 0 || len(host) > 253 {
		return false
	}
	for _, label := range strings.Split(host, ".") {
		if len(label) == 0 || len(label) > 63 || label[0] == '-' || label[len(label)-1] == '-' {
			return false
		}
		for _, c := range label {
			if !(c >= 'a' && c <= 'z' || c >= 'A' && c <= 'Z' || c >= '0' && c <= '9' || c == '-') {
				return false
			}
		}
	}
	return true
}
func operatorConfig(dsn string) (*pgxpool.Config, error) {
	cfg, err := parseStoreConfigFor("operator authority", dsn)
	if err != nil {
		return nil, err
	}
	refuse := errors.New("postgres: operator database transport policy refused")
	pinned := cfg.ConnConfig.RuntimeParams["hostaddr"]
	delete(cfg.ConnConfig.RuntimeParams, "hostaddr")
	if !explicitHost(dsn) {
		if pinned != "" {
			return nil, refuse
		}
		// Match the existing operator's explicit local socket default, independent
		// of PGHOST in the embedding process's environment.
		cfg.ConnConfig.Host = "/var/run/postgresql"
		cfg.ConnConfig.TLSConfig = nil
		cfg.ConnConfig.Fallbacks = nil
	}
	host := cfg.ConnConfig.Host
	if strings.Contains(host, ",") || len(cfg.ConnConfig.Fallbacks) > 0 {
		return nil, refuse
	}
	if strings.HasPrefix(host, "/") {
		if pinned != "" {
			return nil, refuse
		}
	} else {
		if cfg.ConnConfig.TLSConfig == nil || cfg.ConnConfig.TLSConfig.InsecureSkipVerify {
			return nil, refuse
		}
		if pinned != "" && net.ParseIP(pinned) == nil {
			return nil, refuse
		}
		if net.ParseIP(host) == nil && !(dnsIdentity(host) && pinned != "") {
			return nil, refuse
		}
		address := host
		if pinned != "" {
			address = pinned
		}
		cfg.ConnConfig.LookupFunc = func(context.Context, string) ([]string, error) { return []string{address}, nil }
	}
	cfg.ConnConfig.ConnectTimeout = 2 * time.Second
	return cfg, nil
}
