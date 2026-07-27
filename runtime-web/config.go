package main

import (
	"fmt"
	"os"
	"path/filepath"
)

type config struct {
	port       int
	certFile   string
	keyFile    string
	socketPath string
	dbPath     string
	spaPath    string
}

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
		dbPath:     dbPath,
		spaPath:    spaPath,
	}, nil
}

func (c *config) tlsEnabled() bool {
	return c.port != 8080 && c.port != 80
}
