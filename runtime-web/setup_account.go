package main

import (
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/RakuenSoftware/smoothgui/auth"
)

const (
	defaultBootstrapUsername = "aimee"
	setupAccountMinPassword  = 6
)

// setupAccountSystem isolates account mutations so the onboarding transaction
// remains unit-testable without provisioning real system accounts. Backed by
// local PAM accounts (pamAccounts), scoped to the managed login group.
type setupAccountSystem interface {
	Exists(username string) bool
	Create(username, password string) error
	Delete(username string) error
	Lock(username string) error
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

func (s *server) setupAccountBootstrapUser() string {
	return filepath.Join(s.setupAccountDir(), "bootstrap-user")
}

func (s *server) setupAccountCredentials() string {
	return filepath.Join(s.setupAccountDir(), "bootstrap-credentials")
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

// pendingBootstrapUsername identifies only generated credentials that onboarding
// owns and may retire. Explicit first-boot credentials are operator-managed.
func (s *server) pendingBootstrapUsername() (string, bool) {
	if username, ok := readGeneratedBootstrapUsername(s.setupAccountCredentials()); ok {
		return username, true
	}
	// New images persist only the password verifier in the login registry, never
	// the generated plaintext password. The generated username remains safe to
	// identify by its rigid aimee-<12 hex> shape.
	if data, err := os.ReadFile(s.setupAccountStore()); err == nil {
		for _, line := range strings.Split(string(data), "\n") {
			username, _, ok := strings.Cut(line, ":")
			if !ok {
				continue
			}
			suffix := strings.TrimPrefix(username, "aimee-")
			if suffix != username && len(suffix) == 12 {
				if _, err := hex.DecodeString(suffix); err == nil {
					return username, true
				}
			}
		}
	}
	if source, username, ok := readBootstrapUser(s.setupAccountBootstrapUser()); ok &&
		source == "generated" {
		return username, true
	}
	return "", false
}

func readBootstrapUser(path string) (source, username string, ok bool) {
	b, err := os.ReadFile(path)
	if err != nil {
		return "", "", false
	}
	source, username, found := strings.Cut(strings.TrimSpace(string(b)), ":")
	if !found || (source != "explicit" && source != "generated") {
		return "", "", false
	}
	if username != "" {
		if err := auth.ValidateUsername(username); err != nil {
			return "", "", false
		}
	}
	return source, username, true
}

func readReplacementUsername(marker string) (string, bool) {
	b, err := os.ReadFile(marker)
	if err != nil {
		return "", false
	}
	username := strings.TrimSpace(string(b))
	return username, username != ""
}

// writeMarkerFile persists non-secret onboarding state: which account replaced
// the bootstrap login, and which login the image generated. Both are USERNAMES,
// not credentials.
//
// Readable by the whole container on purpose. runtime-web runs as root and the C
// server runs as `aimee`, and the server's roundtable-admin gate resolves the
// administrator from these very files. Written 0600 they were unreadable to it,
// so the gate fell through to its "admin" fallback and 403'd the operator whose
// name was sitting in the file — the exact lockout this marker exists to prevent.
// 0644 leaks nothing: the account name is already visible in every audit line.
func writeMarkerFile(path string, data []byte) error {
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	tmp, err := os.CreateTemp(filepath.Dir(path), ".setup-account-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	defer os.Remove(tmpName)
	if err := tmp.Chmod(0o644); err != nil {
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

// writeReplacementMarker commits only non-secret account state. The password is
// created as a local PAM account first; this marker makes that account
// authoritative. It returns a rollback closure for failures before the old
// sessions have been invalidated.
func writeReplacementMarker(marker, newUsername string) (func(), error) {
	originalMarker, markerErr := os.ReadFile(marker)
	markerExisted := markerErr == nil
	if markerErr != nil && !errors.Is(markerErr, os.ErrNotExist) {
		return nil, markerErr
	}
	rollback := func() {
		if markerExisted {
			_ = writeMarkerFile(marker, originalMarker)
		} else {
			_ = os.Remove(marker)
		}
	}

	if err := writeMarkerFile(marker, []byte(newUsername+"\n")); err != nil {
		return nil, err
	}
	return rollback, nil
}

// retireOnly reports whether `caller` may only RETIRE the pending bootstrap
// login rather than replace it with a new account.
//
// The replacement flow assumes the operator is signed in AS the generated login
// and is trading it for a real one. An operator who instead created their
// account some other way is signed in as that account, and can never satisfy
// that assumption: the generated password lived in bootstrap-credentials, which
// this endpoint deletes on success and which newer images never write at all.
// Observed on the testing appliance — real logins existed, no replacement marker
// did, and the wizard asked forever for a step nobody could complete.
//
// Worse than the nagging: adminUsername() falls back to the pending bootstrap
// user, so the appliance administrator was an account nobody could authenticate
// as, and every policy mutation 403'd for the humans who actually run the box.
//
// So an established managed account may close the generated login instead. That
// only ever REMOVES a way in — the caller already has one — and it is the same
// act the replacement flow performs, minus creating an account.
func (s *server) retireOnly(caller, bootstrapUsername string) bool {
	return caller != "" && caller != bootstrapUsername && s.accounts.Exists(caller)
}

func (s *server) setupAccountStatus(caller string) map[string]any {
	if username, ok := readReplacementUsername(s.setupAccountMarker()); ok {
		return map[string]any{"complete": true, "required": false, "username": username}
	}
	bootstrapUsername, required := s.pendingBootstrapUsername()
	if !required {
		_, username, _ := readBootstrapUser(s.setupAccountBootstrapUser())
		return map[string]any{"complete": true, "required": false, "username": username}
	}
	return map[string]any{
		"complete": false, "required": true, "username": bootstrapUsername,
		// The wizard renders either a create-account form or a retire-this-login
		// confirmation from this, so it never offers an action POST will refuse.
		"retire_only": s.retireOnly(caller, bootstrapUsername),
	}
}

// GET reports whether the temporary generated/legacy login still needs replacement.
// POST creates and persists the replacement account, locks the temporary login,
// invalidates its sessions, and moves this browser to a session for the new user.
func (s *server) handleSetupAccount(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	if r.Method == http.MethodGet {
		// Under OIDC there is no local account to replace, so the wizard must not
		// show the step at all rather than offering an action POST would refuse.
		if s.authMode.oidc(r.Context()) {
			_ = json.NewEncoder(w).Encode(map[string]any{
				"complete": true, "required": false, "managed_by": authModeOIDC,
			})
			return
		}
		_ = json.NewEncoder(w).Encode(s.setupAccountStatus(currentUser(r)))
		return
	}
	if r.Method != http.MethodPost {
		writeJSONError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	// Replacing the bootstrap login creates a LOCAL account. Under OIDC the
	// identity provider owns accounts, so there is nothing here to create and a
	// local one would be a way in that bypasses the IdP's policy.
	if s.requireLocalAccounts(w, r) {
		return
	}
	// This mutation creates a local login. SameSite=Strict already
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
		// Not the bootstrap session. An established managed account may still
		// close the generated login — see retireOnly — but only that, and only
		// when it says so explicitly, so a stray POST cannot retire a login by
		// accident.
		if s.retireOnly(currentUser(r), bootstrapUsername) {
			s.retireBootstrapLogin(w, r, bootstrapUsername)
			return
		}
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
		writeJSONError(w, http.StatusBadRequest,
			fmt.Sprintf("password must be at least %d characters", setupAccountMinPassword))
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
	rollback, err := writeReplacementMarker(s.setupAccountMarker(), req.Username)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "could not commit replacement account state")
		return
	}
	// Invalidate every session authenticated with the bootstrap password. The
	// marker has already made the retired account ineligible for new logins.
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
	// The old account is now unusable and the durable marker commits the
	// replacement. Remove any recoverable legacy plaintext last.
	if err := os.Remove(s.setupAccountCredentials()); err != nil && !errors.Is(err, os.ErrNotExist) {
		fmt.Fprintf(os.Stderr, "setup account: could not remove retired bootstrap credentials: %v\n", err)
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

// retireBootstrapLogin closes the generated login on behalf of an operator who
// is already signed in as a real managed account. It creates nothing: the
// caller's own login is untouched and their session stays valid, so unlike the
// replacement flow there is no account or session to roll back — only the
// marker, if locking then fails.
//
// The caller becomes the appliance administrator, because adminUsername() reads
// this marker. That is the point: before this, the administrator was the
// generated account, which is exactly the lockout adminUsername's own comment
// describes. Whoever runs setup claims it, and root can rewrite the marker to
// hand it elsewhere.
func (s *server) retireBootstrapLogin(w http.ResponseWriter, r *http.Request, bootstrapUsername string) {
	var req struct {
		// Explicit, so an unrelated POST body can never retire a login as a
		// side effect of some other intent.
		RetireBootstrap bool `json:"retire_bootstrap"`
	}
	if err := json.NewDecoder(r.Body).Decode(&req); err != nil {
		writeJSONError(w, http.StatusBadRequest, "invalid request body")
		return
	}
	if !req.RetireBootstrap {
		writeJSONError(w, http.StatusForbidden, "sign in with the bootstrap account to replace it")
		return
	}
	caller := currentUser(r)
	rollback, err := writeReplacementMarker(s.setupAccountMarker(), caller)
	if err != nil {
		writeJSONError(w, http.StatusInternalServerError, "could not commit replacement account state")
		return
	}
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
	if err := os.Remove(s.setupAccountCredentials()); err != nil && !errors.Is(err, os.ErrNotExist) {
		fmt.Fprintf(os.Stderr, "setup account: could not remove retired bootstrap credentials: %v\n", err)
	}
	_ = json.NewEncoder(w).Encode(map[string]any{
		"complete": true, "required": false, "username": caller, "retired": bootstrapUsername,
	})
}

// adminUsername resolves the appliance administrator: the account that replaced
// the generated bootstrap login, or — before that replacement — the bootstrap
// account itself.
//
// The policy gates used to compare against the literal name "admin". That is the
// one account guaranteed NOT to be the administrator on a set-up appliance: the
// documented flow is to replace the generated bootstrap login with an operator
// account, after which "admin" names nothing in particular. An operator who
// completed setup as, say, `virant` was therefore locked out of every roundtable
// and workflow policy mutation on their own appliance, with the browser
// reporting only "administrator access required".
//
// Falls back to "admin" when no record exists at all, which preserves the
// previous behaviour for an appliance that has neither marker.
func (s *server) adminUsername() string {
	// The gate this backs runs on every policy mutation, including paths whose
	// callers hold no config (setupAccountDir dereferences cfg). Never panic a
	// request on the way to a deny.
	if s == nil || s.cfg == nil {
		return "admin"
	}
	if username, ok := readReplacementUsername(s.setupAccountMarker()); ok {
		return username
	}
	if username, ok := s.pendingBootstrapUsername(); ok {
		return username
	}
	if _, username, ok := readBootstrapUser(s.setupAccountBootstrapUser()); ok && username != "" {
		return username
	}
	return "admin"
}

// isAdmin reports whether the authenticated browser identity is the appliance
// administrator. requireAuth injects the username, so it is already trusted.
func (s *server) isAdmin(r *http.Request) bool {
	user := currentUser(r)
	return user != "" && user == s.adminUsername()
}
