package main

import (
	"fmt"
	"os"
	"path/filepath"
	"strings"
)

type config struct {
	port       int
	certFile   string
	keyFile    string
	socketPath string
	wfeEngine  string
	wfeSocket  string
	dbPath     string
	spaPath    string
	// PAM service backing dashboard logins (/etc/pam.d/<name>). Overridable so a
	// deployment can point at its own stack rather than the shipped default.
	pamService string
}

// webchatLoginGroup scopes the accounts the dashboard may see, create or remove,
// so a container system user is never a dashboard login by accident.
const webchatLoginGroup = "aimee-webchat"

const defaultWebchatPAMService = "aimee"

func newConfig(port int, certFile, keyFile, socketPath, dbPath, spaPath string) (*config, error) {
	home, err := os.UserHomeDir()
	if err != nil {
		return nil, fmt.Errorf("home dir: %w", err)
	}
	aimeeDir := filepath.Join(home, ".config", "aimee")

	if socketPath == "" {
		socketPath = filepath.Join(aimeeDir, "aimee.sock")
	}
	if dbPath == "" {
		dbPath = filepath.Join(aimeeDir, "webchat.db")
	}

	return &config{
		port:       port,
		certFile:   certFile,
		keyFile:    keyFile,
		socketPath: socketPath,
		wfeEngine:  strings.TrimSpace(os.Getenv("AIMEE_WFE_ENGINE")),
		wfeSocket:  strings.TrimSpace(os.Getenv("AIMEE_WFE_HTTP_SOCKET")),
		dbPath:     dbPath,
		spaPath:    spaPath,
		pamService: pamServiceFromEnv(),
	}, nil
}

func (c *config) tlsEnabled() bool {
	return c.port != 8080 && c.port != 80
}

// pamServiceFromEnv resolves the PAM service backing dashboard logins.
func pamServiceFromEnv() string {
	if name := strings.TrimSpace(os.Getenv("AIMEE_WEBCHAT_PAM_SERVICE")); name != "" {
		return name
	}
	return defaultWebchatPAMService
}
