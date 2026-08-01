package main

import (
	"errors"
	"fmt"
	"os/exec"
	"os/user"
	"sort"

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
	users   managedUsers
	// Seams: PAM and chpasswd shell out, so tests substitute them.
	authenticate func(service, username, password string) error
	setPassword  func(username, password string) error
	lock         func(username string) error
}

func newPAMAccounts(service, group string) (*pamAccounts, error) {
	if service == "" {
		return nil, errors.New("a PAM service name is required")
	}
	users := auth.NewUserManager(group)
	if err := users.EnsureGroup(); err != nil {
		return nil, fmt.Errorf("managed login group: %w", err)
	}
	return &pamAccounts{
		service:      service,
		users:        users,
		authenticate: auth.PAMAuthenticate,
		setPassword:  auth.SetPassword,
		lock:         lockSystemAccount,
	}, nil
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
	return p.setPassword(username, replacement)
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

// groupLookup is os/user's group lookup, indirected so the collision check can
// be tested without depending on which groups the test host happens to ship.
var groupLookup = func(name string) error {
	_, err := user.LookupGroup(name)
	return err
}

// userLookup reports whether an account of this name already exists.
var userLookup = func(name string) error {
	_, err := user.Lookup(name)
	return err
}

// errUsernameIsGroup explains a collision the operator can actually act on.
//
// useradd allocates a user-private group and fails with "group <name> exists"
// and exit status 9 when one is already there. The server image ships the usual
// Unix groups, so operator, backup, staff, users, news, mail, proxy, adm and
// aimee itself are all taken. That list is not obscure: the wizard's first field
// asks an operator to name their account, and "operator" is the obvious answer.
//
// Without this the shell error reaches the browser verbatim, doubled prefix and
// exit status included, naming neither the real problem nor a way out.
func errUsernameIsGroup(username string) error {
	return fmt.Errorf("%q is already a group on this host, so it cannot also be an account name; choose another", username)
}

func (p *pamAccounts) Create(username, password string) error {
	if err := auth.ValidateUsername(username); err != nil {
		return err
	}
	// Only a collision for a name that is NOT already an account: an existing
	// account owns a like-named private group, and reporting that as a clash
	// would mask the real "user exists" condition the caller handles.
	if userLookup(username) != nil && groupLookup(username) == nil {
		return errUsernameIsGroup(username)
	}
	return p.users.Create(username, password)
}

func (p *pamAccounts) Delete(username string) error {
	return p.users.Delete(username)
}

// Lock disables the bootstrap login once the operator has replaced it. The Vault
// store made this a no-op because no OS login existed; with a real account the
// lock has to actually happen, or the generated first-boot credential keeps
// working forever as a second way in.
func (p *pamAccounts) Lock(username string) error {
	if !p.managed(username) {
		return nil
	}
	return p.lock(username)
}

func lockSystemAccount(username string) error {
	if out, err := exec.Command("usermod", "--lock", "--expiredate", "1", username).CombinedOutput(); err != nil {
		return fmt.Errorf("lock %s: %s: %w", username, string(out), err)
	}
	return nil
}
