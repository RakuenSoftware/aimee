package main

import (
	"errors"
	"fmt"
	"os"
	"os/exec"
	"sort"
	"strings"

	"github.com/RakuenSoftware/smoothgui/auth"
)

// errInvalidWebchatCredential marks a password check that failed for a real
// account, as opposed to a broken authenticator. The auth handler maps it to a
// 401 rather than a 500.
var errInvalidWebchatCredential = errors.New("invalid webchat credential")

// managedUsers is the slice of auth.UserManager this file needs, named as an
// interface so the decision logic is testable without provisioning real system
// accounts. auth.UserManager satisfies it.
type managedUsers interface {
	List() ([]auth.User, error)
	Create(username, password string) error
	Delete(username string) error
	IsManagedUser(username string) bool
}

// pamAccounts authenticates dashboard logins against LOCAL PAM, using the same
// helper SmoothNAS and the kb's /v1/identity/login/pam use. It is the baseline
// identity for the appliance: the wizard's first login happens here, before any
// kb exists to ask about OIDC.
//
// It replaces a Vault-backed credential store. That store existed to solve the
// first-login problem — with no account yet, PAM has nobody to authenticate —
// but it never handed identity back over afterwards, so an appliance stayed on
// bootstrap-shaped credentials forever. The first-boot account is now a real
// system account, so there is one identity system rather than two.
//
// Accounts are scoped to a managed group, so the dashboard can only see, create
// or remove the logins it provisioned, never arbitrary host users.
type pamAccounts struct {
	service string
	group   string
	users   managedUsers
	// Seams: PAM and chpasswd shell out, so tests substitute them.
	authenticate func(service, username, password string) error
	setPassword  func(username, password string) error
	lock         func(username string) error
	// Record the managed accounts after every mutation so they can be restored
	// into a replaced container — see identity_persist.go. A seam because it
	// reads /etc/shadow, which a unit test has no business doing.
	persist func()
}

// aimeeHome is where the appliance's persistent state lives. The entrypoint
// exports AIMEE_HOME; the default matches WEBCHAT_HOME in runtime-web-lib.sh.
func aimeeHome() string {
	if h := os.Getenv("AIMEE_HOME"); h != "" {
		return h
	}
	return "/var/lib/aimee"
}

// recordIdentities snapshots the managed group. Best-effort: an account
// operation must not fail because the record could not be written.
func (p *pamAccounts) recordIdentities() {
	if p.persist != nil {
		p.persist()
	}
}

func newPAMAccounts(service, group string) (*pamAccounts, error) {
	if service == "" {
		return nil, errors.New("a PAM service name is required")
	}
	users := auth.NewUserManager(group)
	if err := users.EnsureGroup(); err != nil {
		return nil, fmt.Errorf("managed login group: %w", err)
	}
	p := &pamAccounts{
		service:      service,
		group:        group,
		users:        users,
		authenticate: auth.PAMAuthenticate,
		setPassword:  auth.SetPassword,
		lock:         lockSystemAccount,
	}
	p.persist = func() {
		names, err := p.List()
		if err != nil {
			return
		}
		_ = snapshotManagedIdentities(aimeeHome(), names, "/etc/shadow")
	}
	return p, nil
}

// Authenticate reports whether the credential is valid.
//
// A PAM stack that is unavailable (missing service file, helper failure) is an
// ERROR, never a silent false: reporting "wrong password" when the authenticator
// itself is broken sends an operator hunting for a credential problem that does
// not exist.
func (p *pamAccounts) Authenticate(username, password string) (bool, error) {
	if username == "" || password == "" {
		return false, nil
	}
	if !p.managed(username) {
		// Refuse host accounts the dashboard did not provision, so a container
		// system user is never a way in.
		return false, nil
	}
	err := p.authenticate(p.service, username, password)
	if err == nil {
		return true, nil
	}
	if errors.Is(err, auth.ErrAuthUnavailable) {
		return false, fmt.Errorf("PAM is unavailable: %w", err)
	}
	return false, nil
}

// UpdatePassword changes a managed account's password, proving the current one
// first so a hijacked session cannot lock the operator out of their own login.
func (p *pamAccounts) UpdatePassword(username, current, replacement string) error {
	ok, err := p.Authenticate(username, current)
	if err != nil {
		return err
	}
	if !ok {
		return errInvalidWebchatCredential
	}
	if err := p.setPassword(username, replacement); err != nil {
		return err
	}
	p.recordIdentities()
	return nil
}

func (p *pamAccounts) List() ([]string, error) {
	users, err := p.users.List()
	if err != nil {
		return nil, err
	}
	names := make([]string, 0, len(users))
	for _, u := range users {
		names = append(names, u.Username)
	}
	sort.Strings(names)
	return names, nil
}

func (p *pamAccounts) managed(username string) bool {
	return p.users.IsManagedUser(username)
}

func (p *pamAccounts) Exists(username string) bool {
	return p.managed(username)
}

// userAdd is indirected so the exact account-creation contract is testable
// without provisioning a real host account.
var userAdd = func(args ...string) ([]byte, error) {
	return exec.Command("useradd", args...).CombinedOutput()
}

var userDelete = func(args ...string) ([]byte, error) {
	return exec.Command("userdel", args...).CombinedOutput()
}

func createManagedSystemUser(group, username, password string,
	setPassword func(string, string) error) error {
	if err := auth.ValidateUsername(username); err != nil {
		return err
	}
	// Name the managed group explicitly as both primary and supplementary.
	// useradd therefore never allocates a same-named private group, so standard
	// names such as operator/backup/aimee no longer collide. The supplementary
	// membership preserves smoothgui/auth's group-member listing.
	out, err := userAdd("--create-home", "--gid", group, "--groups", group,
		"--shell", "/usr/sbin/nologin", username)
	if err != nil {
		return fmt.Errorf("useradd: %s: %w", strings.TrimSpace(string(out)), err)
	}
	if err := setPassword(username, password); err != nil {
		// Match account creation's all-or-nothing contract: a failed chpasswd
		// must not leave a passwordless, managed system account behind. Preserve
		// the password error as the cause and report cleanup failure as context.
		if cleanupOut, cleanupErr := userDelete("--remove", username); cleanupErr != nil {
			return fmt.Errorf("set password: %w (rollback userdel: %s: %v)", err,
				strings.TrimSpace(string(cleanupOut)), cleanupErr)
		}
		return err
	}
	return nil
}

func (p *pamAccounts) Create(username, password string) error {
	if err := auth.ValidateUsername(username); err != nil {
		return err
	}
	if p.group != "" {
		if err := createManagedSystemUser(p.group, username, password, p.setPassword); err != nil {
			return err
		}
	} else if err := p.users.Create(username, password); err != nil {
		return err
	}
	p.recordIdentities()
	return nil
}

func (p *pamAccounts) Delete(username string) error {
	if err := p.users.Delete(username); err != nil {
		return err
	}
	p.recordIdentities()
	return nil
}

// Lock disables the bootstrap login once the operator has replaced it. The Vault
// store made this a no-op because no OS login existed; with a real account the
// lock has to actually happen, or the generated first-boot credential keeps
// working forever as a second way in.
func (p *pamAccounts) Lock(username string) error {
	if !p.managed(username) {
		return nil
	}
	if err := p.lock(username); err != nil {
		return err
	}
	// Re-record so the retired bootstrap account drops out: its verifier is now
	// locked, and restoring it into a fresh container would recreate a login
	// nobody can authenticate as.
	p.recordIdentities()
	return nil
}

func lockSystemAccount(username string) error {
	if out, err := exec.Command("usermod", "--lock", "--expiredate", "1", username).CombinedOutput(); err != nil {
		return fmt.Errorf("lock %s: %s: %w", username, string(out), err)
	}
	return nil
}
