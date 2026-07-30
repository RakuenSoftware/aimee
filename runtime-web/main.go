package main

import (
	"flag"
	"fmt"
	"log"
	"os"
)

func main() {
	port := flag.Int("port", 8443, "HTTPS listen port (use 8080 for plain HTTP)")
	certFile := flag.String("cert", "", "TLS certificate file (PEM); auto-generated if empty")
	keyFile := flag.String("key", "", "TLS key file (PEM); auto-generated if empty")
	socketPath := flag.String("socket", "", "aimee-server Unix socket path (default: ~/.config/aimee/aimee.sock)")
	dbPath := flag.String("db", "", "Session database path (default: ~/.config/aimee/webchat.db)")
	spaPath := flag.String("spa", "", "Path to built frontend/dist/index.html (auto-discovered if empty)")
	flag.Parse()

	cfg, err := newConfig(*port, *certFile, *keyFile, *socketPath, *dbPath, *spaPath)
	if err != nil {
		fmt.Fprintf(os.Stderr, "aimee-runtime-web: config: %v\n", err)
		os.Exit(1)
	}

	log.Printf("aimee-runtime-web: starting on :%d", cfg.port)
	if err := run(cfg); err != nil {
		fmt.Fprintf(os.Stderr, "aimee-runtime-web: %v\n", err)
		os.Exit(1)
	}
}
