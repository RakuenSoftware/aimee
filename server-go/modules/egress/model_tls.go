package egress

import (
	"crypto/tls"
	"crypto/x509"
	"errors"
	"net/url"
	"os"
	"path/filepath"
)

// Select a local identity from the authorized purpose and exact service origin.
// Callers cannot supply key paths, trust roots or a profile override. Redirects
// are not followed by executeHTTP, so the client identity cannot escape this hop.
func modelTLSConfig(target *url.URL, purpose, root string) (*tls.Config, error) {
	config := &tls.Config{MinVersion: tls.VersionTLS12, ServerName: target.Hostname()}
	profile := ""
	switch target.Hostname() {
	case "aimee-embedder":
		if target.Scheme != "https" || target.Host != "aimee-embedder:8762" || (purpose != "embedding" && purpose != "embedding-health") {
			return nil, errors.New("invalid local embedding origin or purpose")
		}
		profile = "embedding"
	case "aimee-llm":
		if target.Scheme != "https" || target.Host != "aimee-llm:8761" || purpose != "provider" {
			return nil, errors.New("invalid local synthesis origin or purpose")
		}
		profile = "synthesis"
	default:
		return config, nil
	}
	dir := filepath.Join(root, profile, "client")
	ca, err := os.ReadFile(filepath.Join(dir, "ca.pem"))
	if err != nil {
		return nil, errors.New("local model CA is unavailable")
	}
	config.RootCAs = x509.NewCertPool()
	if !config.RootCAs.AppendCertsFromPEM(ca) {
		return nil, errors.New("invalid local model CA")
	}
	cert, err := tls.LoadX509KeyPair(filepath.Join(dir, "client.pem"), filepath.Join(dir, "client.key"))
	if err != nil {
		return nil, errors.New("local model client identity is unavailable")
	}
	config.Certificates = []tls.Certificate{cert}
	return config, nil
}
