package main

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"strings"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

const (
	defaultBootstrapUsername = "aimee"
	defaultBootstrapPassword = "aimee-local-dev"
	setupAccountMinPassword  = 8
)

// setupAccountSystem isolates the root-only PAM mutations so the onboarding
// transaction is unit-testable without changing accounts on the test host.
type setupAccountSystem interface {
	Exists(username string) bool
	Create(username, password string) error
	Delete(username string) error
	ShadowHash(username string) (string, error)
	Lock(username string) error
}

type osSetupAccountSystem struct {
	users *auth.UserManager
}

func (m osSetupAccountSystem) Exists(username string) bool {
	return auth.UserExists(username)
}

func (m osSetupAccountSystem) Create(username, password string) error {
	return m.users.Create(username, password)
}

func (m osSetupAccountSystem) Delete(username string) error {
	return m.users.Delete(username)
}

func (m osSetupAccountSystem) ShadowHash(username string) (string, error) {
	out, err := exec.Command("getent", "shadow", username).Output()
	if err != nil {
		return "", fmt.Errorf("read password hash: %w", err)
	}
	parts := strings.SplitN(strings.TrimSpace(string(out)), ":", 3)
	if len(parts) < 2 || parts[1] == "" || strings.HasPrefix(parts[1], "!") ||
		strings.HasPrefix(parts[1], "*") {
		return "", errors.New("new account has no usable password hash")
	}
	return parts[1], nil
}

func (m osSetupAccountSystem) Lock(username string) error {
	if out, err := exec.Command("usermod", "--lock", username).CombinedOutput(); err != nil {
		return fmt.Errorf("lock bootstrap login: %s: %w", strings.TrimSpace(string(out)), err)
	}
	return nil
}

func (s *server) setupAccountDir() string {
	return filepath.Join(filepath.Dir(s.cfg.dbPath), "webchat")
}

func (s *server) setupAccountMarker() string {
	return filepath.Join(s.setupAccountDir(), "bootstrap-replaced")
}

func (s *server) setupAccountStore() string {
	return filepath.Join(s.setupAccountDir(), "logins")
}

func (s *server) setupAccountCredentials() string {
	return filepath.Join(s.setupAccountDir(), "bootstrap-credentials")
}

// configuredWithKnownBootstrap returns true only for the published development
// credential. Operators who explicitly inject a different login do not need the
// replacement step and must not have their account retired by it.
func configuredWithKnownBootstrap() bool {
	return os.Getenv("AIMEE_WEBCHAT_USER") == defaultBootstrapUsername &&
		os.Getenv("AIMEE_WEBCHAT_PASSWORD") == defaultBootstrapPassword
}

// readGeneratedBootstrapUsername reads the root-only file written by the image
// entrypoint when no explicit browser credential pair was supplied. Validate the
// complete shape even though this endpoint needs only the username: a corrupt or
// partially-written file must never authorize retirement of an unrelated user.
func readGeneratedBootstrapUsername(path string) (string, bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", false
	}
	var username, password string
	for _, line := range strings.Split(string(b), "\n") {
		if strings.HasPrefix(line, "username=") {
			if username != "" {
				return "", false
			}
			username = strings.TrimPrefix(line, "username=")
			continue
		}
		if strings.HasPrefix(line, "password=") {
			if password != "" {
				return "", false
			}
			password = strings.TrimPrefix(line, "password=")
			continue
		}
		if line != "" {
			return "", false
		}
	}
	suffix := strings.TrimPrefix(username, "aimee-")
	if suffix == username || len(suffix) != 12 || len(password) != 64 {
		return "", false
	}
	if _, err := hex.DecodeString(suffix); err != nil {
		return "", false
	}
	if _, err := hex.DecodeString(password); err != nil {
		return "", false
	}
	return username, true
}

// pendingBootstrapUsername identifies only credentials that onboarding owns and
// may retire. Custom env credentials are operator-managed and skip replacement.
// The legacy published pair remains recognized for rolling-upgrade safety.
func (s *server) pendingBootstrapUsername() (string, bool) {
	if username, ok := readGeneratedBootstrapUsername(s.setupAccountCredentials()); ok {
		return username, true
	}
	if configuredWithKnownBootstrap() {
		return defaultBootstrapUsername, true
	}
	return "", false
}

func readReplacementUsername(marker string) (string, bool) {
	b, err := os.ReadFile(marker)
	if err != nil {
		return "", false
	}
	username := strings.TrimSpace(string(b))
	return username, username != ""
}

func writePrivateFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".setup-account-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0o600); err != nil {
		tmp.Close()
		return err
	}
	if _, err := tmp.Write(data); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Sync(); err != nil {
		tmp.Close()
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	return os.Rename(tmpName, path)
}

// durableLoginReplacement rewrites the entrypoint-compatible username:crypt-hash
// registry and records the bootstrap retirement marker. It returns a rollback
// closure because the live account lock happens after persistence succeeds.
func durableLoginReplacement(store, marker, credentials, oldUsername, newUsername,
	newHash string) (func(), error) {
	originalStore, storeErr := os.ReadFile(store)
	storeExisted := storeErr == nil
	if storeErr != nil && !errors.Is(storeErr, os.ErrNotExist) {
		return nil, storeErr
	}
	originalMarker, markerErr := os.ReadFile(marker)
	markerExisted := markerErr == nil
	if markerErr != nil && !errors.Is(markerErr, os.ErrNotExist) {
		return nil, markerErr
	}
	originalCredentials, credentialsErr := os.ReadFile(credentials)
	credentialsExisted := credentialsErr == nil
	if credentialsErr != nil && !errors.Is(credentialsErr, os.ErrNotExist) {
		return nil, credentialsErr
	}

	rollback := func() {
		if storeExisted {
			_ = writePrivateFile(store, originalStore)
		} else {
			_ = os.Remove(store)
		}
		if markerExisted {
			_ = writePrivateFile(marker, originalMarker)
		} else {
			_ = os.Remove(marker)
		}
		if credentialsExisted {
			_ = writePrivateFile(credentials, originalCredentials)
		} else {
			_ = os.Remove(credentials)
		}
	}

	lines := make([]string, 0)
	for _, line := range strings.Split(string(originalStore), "\n") {
		if line == "" || strings.HasPrefix(line, oldUsername+":") ||
			strings.HasPrefix(line, newUsername+":") {
			continue
		}
		lines = append(lines, line)
	}
	lines = append(lines, newUsername+":"+newHash)
	updatedStore := []byte(strings.Join(lines, "\n") + "\n")
	if err := writePrivateFile(store, updatedStore); err != nil {
		return nil, err
	}
	if err := writePrivateFile(marker, []byte(newUsername+"\n")); err != nil {
		rollback()
		return nil, err
	}
	if err := os.Remove(credentials); err != nil && !errors.Is(err, os.ErrNotExist) {
		rollback()
		return nil, err
	}
	return rollback, nil
}

func (s *server) setupAccountStatus() map[string]any {
	if username, ok := readReplacementUsername(s.setupAccountMarker()); ok {
		return map[string]any{"complete": true, "required": false, "username": username}
	}
	bootstrapUsername, required := s.pendingBootstrapUsername()
	if !required {
		return map[string]any{"complete": true, "required": false, "username": currentConfiguredLogin()}
	}
	return map[string]any{"complete": false, "required": true, "username": bootstrapUsername}
}

func currentConfiguredLogin() string {
	return strings.TrimSpace(os.Getenv("AIMEE_WEBCHAT_USER"))
}

// GET reports whether the temporary generated/legacy login still needs replacement.
// POST creates and persists the replacement account, locks the temporary login,
// invalidates its sessions, and moves this browser to a session for the new user.
func (s *server) handleSetupAccount(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method == http.MethodGet {
		_ = json.NewEncoder(w).Encode(s.setupAccountStatus())
		return
	}
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	// This mutation changes a root-managed PAM account. SameSite=Strict already
	// protects the session cookie in browsers; keep an explicit origin gate here
	// as defense in depth for the highest-impact onboarding request.
	if !sameOriginRequest(r) {
		writeJSONError(w, http.StatusForbidden, "cross-origin account setup refused")
		return
	}
	bootstrapUsername, pending := s.pendingBootstrapUsername()
	if _, complete := readReplacementUsername(s.setupAccountMarker()); complete || !pending {
		writeJSONError(w, http.StatusConflict, "bootstrap account has already been replaced")
		return
	}
	if currentUser(r) != bootstrapUsername {
		writeJSONError(w, http.StatusForbidden, "sign in with the bootstrap account to replace it")
		return
	}

	var req struct {
		Username             string `json:"username"`
		Password             string `json:"password"`
		PasswordConfirmation string `json:"password_confirmation"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid request body")
		return
	}
	req.Username = strings.TrimSpace(req.Username)
	if err := auth.ValidateUsername(req.Username); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	if req.Username == bootstrapUsername {
		writeJSONError(w, http.StatusBadRequest, "choose a username other than the bootstrap login")
		return
	}
	if len(req.Password) < setupAccountMinPassword {
		writeJSONError(w, http.StatusBadRequest, "password must be at least 8 characters")
		return
	}
	if strings.ContainsAny(req.Password, "\x00\r\n") {
		writeJSONError(w, http.StatusBadRequest, "password contains an unsupported control character")
		return
	}
	if req.Password != req.PasswordConfirmation {
		writeJSONError(w, http.StatusBadRequest, "passwords do not match")
		return
	}
	if s.accounts.Exists(req.Username) {
		writeJSONError(w, http.StatusConflict, "username already exists")
		return
	}

	if err := s.accounts.Create(req.Username, req.Password); err != nil {
		writeJSONError(w, http.StatusBadRequest, err.Error())
		return
	}
	cleanupUser := true
	defer func() {
		if cleanupUser {
			_ = s.accounts.Delete(req.Username)
		}
	}()

	newHash, err := s.accounts.ShadowHash(req.Username)
	if err != nil || strings.ContainsAny(newHash, ":\r\n") {
		if err == nil {
			err = errors.New("invalid password hash")
		}
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}
	newToken, err := s.sessions.CreateSession(req.Username)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "could not create replacement session")
		return
	}
	cleanupSession := true
	defer func() {
		if cleanupSession {
			_ = s.sessions.DeleteSession(newToken)
		}
	}()
	rollback, err := durableLoginReplacement(s.setupAccountStore(), s.setupAccountMarker(),
		s.setupAccountCredentials(), bootstrapUsername, req.Username, newHash)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "could not persist replacement account")
		return
	}
	// Invalidate every session authenticated with the published password before
	// locking that login. A failure rolls durable state back while the PAM login
	// still works, so the operator can sign in and retry.
	if err := s.sessions.DeleteSessionsForUser(bootstrapUsername); err != nil {
		rollback()
		writeJSONError(w, http.StatusInternalServerError, "could not invalidate bootstrap sessions")
		return
	}
	if err := s.accounts.Lock(bootstrapUsername); err != nil {
		rollback()
		writeJSONError(w, http.StatusInternalServerError, err.Error())
		return
	}

	http.SetCookie(w, &http.Cookie{
		Name:     "session",
		Value:    newToken,
		Path:     "/",
		HttpOnly: true,
		Secure:   s.cfg.tlsEnabled(),
		SameSite: http.SameSiteStrictMode,
		MaxAge:   int((24 * time.Hour) / time.Second),
	})
	cleanupSession = false
	cleanupUser = false
	_ = json.NewEncoder(w).Encode(map[string]any{
		"complete": true, "required": false, "username": req.Username,
	})
}
