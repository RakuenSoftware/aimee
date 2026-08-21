package main

import (
	"bytes"
	"encoding/base64"
	"errors"
	"os/exec"
	"strings"
	"sync"
)

const maxWebchatVaultExport = 512 * 1024

type webchatVaultSnapshot struct {
	PrimaryUser   string
	PrimaryPass   string
	ExtraUsers    string
	LegacyPrimary string
	LegacyHashes  string
	AccountsJSON  string
	SessionHMAC   string
	TLSKey        string
}

type webchatVaultStore interface {
	Snapshot() (webchatVaultSnapshot, error)
	Seal(record string, value []byte) error
}

// commandWebchatVault is deliberately not a general Vault client. The C helper
// exports and mutates only the fixed webchat-login records needed by this
// service. Secret bytes use stdout/stdin pipes and never argv, environment, or a
// temporary file.
type commandWebchatVault struct {
	mu sync.Mutex
}

// aimeeServerPath resolves aimee-server in THIS process's PATH before handing it
// to runuser. runuser replaces the environment with the target user's login
// PATH, which on a stock Debian login.defs is /sbin:/bin:/usr/sbin:/usr/bin --
// and aimee-server installs to /usr/local/bin, including in the server image.
// Passing the bare name made the lookup depend on the aimee user's login PATH
// rather than on where the binary actually is, and where that PATH omits the
// install directory the service exits at startup with "webchat Vault export
// failed" -- an error about the Vault, for a binary that was never found.
// Resolving here changes nothing where the bare name already worked.
func aimeeServerPath() string {
	if resolved, err := exec.LookPath("aimee-server"); err == nil {
		return resolved
	}
	return "aimee-server"
}

func (v *commandWebchatVault) Snapshot() (webchatVaultSnapshot, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	cmd := exec.Command("runuser", "-u", "aimee", "--", aimeeServerPath(), "--webchat-vault-export")
	var out bytes.Buffer
	cmd.Stdout = &out
	cmd.Stderr = new(bytes.Buffer)
	if err := cmd.Run(); err != nil {
		return webchatVaultSnapshot{}, errors.New("webchat Vault export failed")
	}
	if out.Len() > maxWebchatVaultExport {
		return webchatVaultSnapshot{}, errors.New("webchat Vault export exceeded limit")
	}
	return parseWebchatVaultExport(out.String())
}

func (v *commandWebchatVault) Seal(record string, value []byte) error {
	v.mu.Lock()
	defer v.mu.Unlock()
	cmd := exec.Command("runuser", "-u", "aimee", "--", aimeeServerPath(), "--webchat-vault-seal", record)
	cmd.Stdin = bytes.NewReader(value)
	cmd.Stdout = new(bytes.Buffer)
	cmd.Stderr = new(bytes.Buffer)
	if err := cmd.Run(); err != nil {
		return errors.New("webchat Vault write failed")
	}
	return nil
}

func parseWebchatVaultExport(raw string) (webchatVaultSnapshot, error) {
	var snap webchatVaultSnapshot
	seen := map[string]bool{}
	for _, line := range strings.Split(strings.TrimSpace(raw), "\n") {
		if line == "" {
			continue
		}
		label, encoded, ok := strings.Cut(line, "\t")
		if !ok || label == "" || encoded == "" || seen[label] {
			return snap, errors.New("invalid webchat Vault export")
		}
		seen[label] = true
		value, err := base64.StdEncoding.DecodeString(encoded)
		if err != nil || bytes.IndexByte(value, 0) >= 0 {
			return snap, errors.New("invalid webchat Vault record")
		}
		s := string(value)
		for i := range value {
			value[i] = 0
		}
		switch label {
		case "user":
			snap.PrimaryUser = s
		case "password":
			snap.PrimaryPass = s
		case "users":
			snap.ExtraUsers = s
		case "legacy_primary":
			snap.LegacyPrimary = s
		case "legacy_hashes":
			snap.LegacyHashes = s
		case "accounts":
			snap.AccountsJSON = s
		case "session_hmac":
			snap.SessionHMAC = s
		case "tls_key":
			snap.TLSKey = s
		default:
			return snap, errors.New("unknown webchat Vault record")
		}
	}
	return snap, nil
}
