package main

import (
	"errors"
	"strings"
	"testing"

	"github.com/RakuenSoftware/smoothgui/auth"
)

type fakeUsers struct {
	members  map[string]bool
	created  map[string]string
	deleted  []string
	listErr  error
	createEr error
}

func newFakeUsers(members ...string) *fakeUsers {
	f := &fakeUsers{members: map[string]bool{}, created: map[string]string{}}
	for _, m := range members {
		f.members[m] = true
	}
	return f
}

func (f *fakeUsers) List() ([]auth.User, error) {
	if f.listErr != nil {
		return nil, f.listErr
	}
	out := []auth.User{}
	for name := range f.members {
		out = append(out, auth.User{Username: name})
	}
	return out, nil
}

func (f *fakeUsers) Create(username, password string) error {
	if f.createEr != nil {
		return f.createEr
	}
	f.created[username] = password
	f.members[username] = true
	return nil
}

func (f *fakeUsers) Delete(username string) error {
	f.deleted = append(f.deleted, username)
	delete(f.members, username)
	return nil
}

func (f *fakeUsers) IsManagedUser(username string) bool { return f.members[username] }

func newTestPAM(users managedUsers, authFn func(string, string, string) error) *pamAccounts {
	return &pamAccounts{
		service:      "aimee",
		users:        users,
		authenticate: authFn,
		setPassword:  func(string, string) error { return nil },
		lock:         func(string) error { return nil },
	}
}

// The dashboard must only accept the logins it provisioned. A container carries
// plenty of system accounts (root, aimee, daemon…); if PAM were consulted for
// any of them, every one would become a way into the dashboard.
func TestPAMAuthenticateRefusesUnmanagedAccountsWithoutConsultingPAM(t *testing.T) {
	called := false
	p := newTestPAM(newFakeUsers("virant"), func(string, string, string) error {
		called = true
		return nil
	})

	ok, err := p.Authenticate("root", "correct-horse")
	if err != nil || ok {
		t.Fatalf("Authenticate(root) = %v, %v; want false", ok, err)
	}
	if called {
		t.Fatal("PAM was consulted for an unmanaged account")
	}

	ok, err = p.Authenticate("virant", "correct-horse")
	if err != nil || !ok {
		t.Fatalf("Authenticate(virant) = %v, %v; want true", ok, err)
	}
}

// A broken PAM stack — no service file, helper missing — must surface as an
// error. Reporting "wrong password" would send an operator hunting a credential
// problem that does not exist, which is exactly how this appliance's identity
// bug stayed hidden.
func TestPAMUnavailableIsAnErrorNotABadPassword(t *testing.T) {
	p := newTestPAM(newFakeUsers("virant"), func(string, string, string) error {
		return auth.ErrAuthUnavailable
	})
	ok, err := p.Authenticate("virant", "whatever")
	if ok {
		t.Fatal("an unavailable PAM stack must not authenticate")
	}
	if err == nil || !errors.Is(err, auth.ErrAuthUnavailable) {
		t.Fatalf("err = %v; want ErrAuthUnavailable", err)
	}
}

func TestPAMAuthenticateRejectsEmptyCredentials(t *testing.T) {
	p := newTestPAM(newFakeUsers("virant"), func(string, string, string) error { return nil })
	for _, tc := range [][2]string{{"", "pw"}, {"virant", ""}, {"", ""}} {
		if ok, err := p.Authenticate(tc[0], tc[1]); ok || err != nil {
			t.Fatalf("Authenticate(%q,%q) = %v, %v; want false", tc[0], tc[1], ok, err)
		}
	}
}

// UpdatePassword must prove the current credential first, so a hijacked session
// cannot lock the operator out of their own appliance.
func TestPAMUpdatePasswordProvesTheCurrentCredential(t *testing.T) {
	users := newFakeUsers("virant")
	set := ""
	p := newTestPAM(users, func(_, _, password string) error {
		if password != "right" {
			return errors.New("denied")
		}
		return nil
	})
	p.setPassword = func(_, password string) error { set = password; return nil }

	if err := p.UpdatePassword("virant", "wrong", "new"); err == nil {
		t.Fatal("a wrong current password must not change the credential")
	}
	if set != "" {
		t.Fatalf("password was changed despite a failed check: %q", set)
	}
	if err := p.UpdatePassword("virant", "right", "new"); err != nil {
		t.Fatalf("UpdatePassword = %v; want nil", err)
	}
	if set != "new" {
		t.Fatalf("set = %q; want new", set)
	}
}

// Locking the retired bootstrap login has to actually happen. The Vault store
// made Lock a no-op because no OS login existed, which left the generated
// first-boot credential working forever as a second way in.
func TestPAMLockDisablesTheRetiredBootstrapAccount(t *testing.T) {
	users := newFakeUsers("aimee-0123456789ab", "virant")
	locked := ""
	p := newTestPAM(users, func(string, string, string) error { return nil })
	p.lock = func(username string) error { locked = username; return nil }

	if err := p.Lock("aimee-0123456789ab"); err != nil {
		t.Fatalf("Lock = %v", err)
	}
	if locked != "aimee-0123456789ab" {
		t.Fatalf("locked = %q; want the bootstrap account", locked)
	}

	// An account this dashboard does not manage is never touched.
	locked = ""
	if err := p.Lock("root"); err != nil {
		t.Fatalf("Lock(root) = %v; want nil", err)
	}
	if locked != "" {
		t.Fatalf("locked an unmanaged account: %q", locked)
	}
}

// List reports only managed logins, so the dashboard's user list can never
// expose the container's system accounts.
func TestPAMListReportsOnlyManagedLogins(t *testing.T) {
	p := newTestPAM(newFakeUsers("virant", "admin"), func(string, string, string) error { return nil })
	names, err := p.List()
	if err != nil {
		t.Fatal(err)
	}
	if len(names) != 2 || names[0] != "admin" || names[1] != "virant" {
		t.Fatalf("List() = %v; want sorted [admin virant]", names)
	}
}

// A username that already names a host group must be refused with a sentence an
// operator can act on, instead of useradd's "group X exists ... exit status 9"
// reaching the browser. The image ships operator, backup, staff, users and aimee
// as groups, and the wizard's first field is where someone meets them.
func TestCreateRefusesUsernameThatIsAlreadyAGroup(t *testing.T) {
	origGroup, origUser := groupLookup, userLookup
	defer func() { groupLookup, userLookup = origGroup, origUser }()

	users := &fakeUsers{members: map[string]bool{}, created: map[string]string{}}
	p := &pamAccounts{users: users}

	// "operator" is a group and not an account.
	groupLookup = func(name string) error {
		if name == "operator" {
			return nil
		}
		return errors.New("no such group")
	}
	userLookup = func(string) error { return errors.New("no such user") }

	err := p.Create("operator", "irrelevant")
	if err == nil {
		t.Fatal("expected a refusal for a username that is already a group")
	}
	if !strings.Contains(err.Error(), "already a group") {
		t.Fatalf("error should name the collision, got: %v", err)
	}
	if len(users.created) != 0 {
		t.Fatalf("must not reach the user manager, created=%v", users.created)
	}

	// A free name still goes through.
	if err := p.Create("admin", "irrelevant"); err != nil {
		t.Fatalf("a name that is not a group should be created: %v", err)
	}

	// An EXISTING account owns a like-named private group. That must not be
	// reported as a collision, or the caller's real "user exists" path is masked.
	groupLookup = func(string) error { return nil }
	userLookup = func(string) error { return nil }
	if err := p.Create("existing", "irrelevant"); err != nil {
		t.Fatalf("an existing account must not be refused as a group clash: %v", err)
	}
}
