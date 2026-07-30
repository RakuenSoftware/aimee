package main

import (
	"bytes"
	"crypto/sha256"
	"crypto/subtle"
	"encoding/base64"
	"encoding/json"
	"errors"
	"os"
	"os/exec"
	"sort"
	"strings"
	"sync"

	"github.com/RakuenSoftware/smoothgui/auth"
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

func (v *commandWebchatVault) Snapshot() (webchatVaultSnapshot, error) {
	v.mu.Lock()
	defer v.mu.Unlock()
	cmd := exec.Command("runuser", "-u", "aimee", "--", "aimee-server", "--webchat-vault-export")
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
	cmd := exec.Command("runuser", "-u", "aimee", "--", "aimee-server", "--webchat-vault-seal", record)
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

type vaultAccounts struct {
	vault  webchatVaultStore
	marker string
	mu     sync.Mutex
}

func newVaultAccounts(vault webchatVaultStore, marker string) *vaultAccounts {
	return &vaultAccounts{vault: vault, marker: marker}
}

func secureEqual(a, b string) bool {
	ha := sha256.Sum256([]byte(a))
	hb := sha256.Sum256([]byte(b))
	return subtle.ConstantTimeCompare(ha[:], hb[:]) == 1
}

func parseAccountJSON(raw string) (map[string]string, error) {
	accounts := map[string]string{}
	if raw == "" {
		return accounts, nil
	}
	if err := json.Unmarshal([]byte(raw), &accounts); err != nil {
		return nil, errors.New("invalid Vault account registry")
	}
	for username, password := range accounts {
		if err := auth.ValidateUsername(username); err != nil || password == "" ||
			strings.ContainsAny(password, "\x00\r\n") {
			return nil, errors.New("invalid Vault account registry")
		}
	}
	return accounts, nil
}

func addPlainAccount(accounts map[string]string, entry string) {
	username, password, ok := strings.Cut(entry, ":")
	if !ok || auth.ValidateUsername(strings.TrimSpace(username)) != nil || password == "" {
		return
	}
	accounts[strings.TrimSpace(username)] = password
}

func effectivePlainAccounts(snap webchatVaultSnapshot) (map[string]string, error) {
	accounts, err := parseAccountJSON(snap.AccountsJSON)
	if err != nil {
		return nil, err
	}
	// Vault-managed account updates override immutable first-boot records.
	overrides := make(map[string]bool, len(accounts))
	for username := range accounts {
		overrides[username] = true
	}
	if snap.PrimaryUser != "" || snap.PrimaryPass != "" {
		if snap.PrimaryUser == "" || snap.PrimaryPass == "" || auth.ValidateUsername(snap.PrimaryUser) != nil {
			return nil, errors.New("incomplete first-boot webchat credential")
		}
		if !overrides[snap.PrimaryUser] {
			accounts[snap.PrimaryUser] = snap.PrimaryPass
		}
	}
	for _, entry := range strings.FieldsFunc(snap.ExtraUsers, func(r rune) bool { return r == ',' || r == '\n' }) {
		entry = strings.TrimSpace(entry)
		username, _, _ := strings.Cut(entry, ":")
		username = strings.TrimSpace(username)
		if !overrides[username] {
			addPlainAccount(accounts, entry)
		}
	}
	if username, _, ok := strings.Cut(snap.LegacyPrimary, ":"); !overrides[username] && ok {
		addPlainAccount(accounts, snap.LegacyPrimary)
	}
	return accounts, nil
}

func legacyHashes(raw string) map[string]string {
	out := map[string]string{}
	for _, line := range strings.Split(raw, "\n") {
		username, hash, ok := strings.Cut(strings.TrimSpace(line), ":")
		if ok && auth.ValidateUsername(username) == nil && hash != "" {
			out[username] = hash
		}
	}
	return out
}

func (a *vaultAccounts) replacementUsername() string {
	b, err := os.ReadFile(a.marker)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(b))
}

func (a *vaultAccounts) Authenticate(username, password string) (bool, error) {
	snap, err := a.vault.Snapshot()
	if err != nil {
		return false, err
	}
	plain, err := effectivePlainAccounts(snap)
	if err != nil {
		return false, err
	}
	if replacement := a.replacementUsername(); replacement != "" {
		if username != replacement {
			return false, nil
		}
		stored, ok := plain[replacement]
		return ok && secureEqual(stored, password), nil
	}
	if stored, ok := plain[username]; ok {
		return secureEqual(stored, password), nil
	}
	if hash, ok := legacyHashes(snap.LegacyHashes)[username]; ok {
		return verifyLegacyPassword(password, hash), nil
	}
	return false, nil
}

func (a *vaultAccounts) snapshotAccounts() (webchatVaultSnapshot, map[string]string, error) {
	snap, err := a.vault.Snapshot()
	if err != nil {
		return snap, nil, err
	}
	accounts, err := parseAccountJSON(snap.AccountsJSON)
	return snap, accounts, err
}

func (a *vaultAccounts) sealAccounts(accounts map[string]string) error {
	data, err := json.Marshal(accounts)
	if err != nil {
		return err
	}
	defer func() {
		for i := range data {
			data[i] = 0
		}
	}()
	return a.vault.Seal("accounts", data)
}

func (a *vaultAccounts) Exists(username string) bool {
	names, err := a.List()
	if err != nil {
		return false
	}
	for _, name := range names {
		if name == username {
			return true
		}
	}
	return false
}

func (a *vaultAccounts) Create(username, password string) error {
	if err := auth.ValidateUsername(username); err != nil {
		return err
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	_, accounts, err := a.snapshotAccounts()
	if err != nil {
		return err
	}
	if _, exists := accounts[username]; exists {
		return errors.New("username already exists")
	}
	accounts[username] = password
	return a.sealAccounts(accounts)
}

func (a *vaultAccounts) Delete(username string) error {
	a.mu.Lock()
	defer a.mu.Unlock()
	_, accounts, err := a.snapshotAccounts()
	if err != nil {
		return err
	}
	delete(accounts, username)
	return a.sealAccounts(accounts)
}

func (a *vaultAccounts) Lock(string) error {
	// The replacement marker atomically switches authentication to the new
	// Vault account. No OS login exists to lock.
	return nil
}

func (a *vaultAccounts) UpdatePassword(username, current, replacement string) error {
	ok, err := a.Authenticate(username, current)
	if err != nil {
		return err
	}
	if !ok {
		return errInvalidWebchatCredential
	}
	a.mu.Lock()
	defer a.mu.Unlock()
	_, accounts, err := a.snapshotAccounts()
	if err != nil {
		return err
	}
	accounts[username] = replacement
	return a.sealAccounts(accounts)
}

func (a *vaultAccounts) List() ([]string, error) {
	snap, err := a.vault.Snapshot()
	if err != nil {
		return nil, err
	}
	plain, err := effectivePlainAccounts(snap)
	if err != nil {
		return nil, err
	}
	if replacement := a.replacementUsername(); replacement != "" {
		if _, ok := plain[replacement]; !ok {
			return nil, errors.New("replacement account is missing from Vault")
		}
		return []string{replacement}, nil
	}
	for username := range legacyHashes(snap.LegacyHashes) {
		if _, overridden := plain[username]; !overridden {
			plain[username] = ""
		}
	}
	names := make([]string, 0, len(plain))
	for username := range plain {
		names = append(names, username)
	}
	sort.Strings(names)
	return names, nil
}

var errInvalidWebchatCredential = errors.New("invalid webchat credential")
