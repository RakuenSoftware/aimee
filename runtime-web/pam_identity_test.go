package main

import (
	"errors"
	"reflect"
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

// A username that already names a host group remains creatable because the
// wizard names its managed primary group instead of asking useradd to allocate a
// colliding private group.
func TestCreateUsesManagedPrimaryGroup(t *testing.T) {
	orig := userAdd
	origDelete := userDelete
	defer func() { userAdd = orig; userDelete = origDelete }()
	var args []string
	userAdd = func(got ...string) ([]byte, error) {
		args = append([]string(nil), got...)
		return nil, nil
	}
	passwordSet := false
	p := &pamAccounts{
		group: "aimee-webchat",
		users: newFakeUsers(),
		setPassword: func(username, password string) error {
			passwordSet = username == "operator" && password == "secret"
			return nil
		},
	}
	if err := p.Create("operator", "secret"); err != nil {
		t.Fatalf("Create(operator) = %v", err)
	}
	want := []string{"--create-home", "--gid", "aimee-webchat", "--groups", "aimee-webchat", "--shell", "/usr/sbin/nologin", "operator"}
	if !reflect.DeepEqual(args, want) {
		t.Fatalf("useradd args = %v; want %v", args, want)
	}
	if !passwordSet {
		t.Fatal("created account password was not set")
	}
}

func TestCreateRollsBackAccountWhenPasswordSetupFails(t *testing.T) {
	orig := userAdd
	origDelete := userDelete
	defer func() { userAdd = orig; userDelete = origDelete }()
	userAdd = func(...string) ([]byte, error) { return nil, nil }
	var deleted []string
	userDelete = func(args ...string) ([]byte, error) {
		deleted = append([]string(nil), args...)
		return nil, nil
	}
	wantErr := errors.New("chpasswd failed")
	p := &pamAccounts{
		group: "aimee-webchat", users: newFakeUsers(),
		setPassword: func(string, string) error { return wantErr },
	}
	if err := p.Create("operator", "secret"); !errors.Is(err, wantErr) {
		t.Fatalf("Create error = %v; want %v", err, wantErr)
	}
	if want := []string{"--remove", "operator"}; !reflect.DeepEqual(deleted, want) {
		t.Fatalf("userdel args = %v; want %v", deleted, want)
	}
}

func TestCreateSurfacesUseraddDiagnostic(t *testing.T) {
	orig := userAdd
	defer func() { userAdd = orig }()
	userAdd = func(...string) ([]byte, error) {
		return []byte("useradd: user 'operator' already exists"), errors.New("exit status 9")
	}
	p := &pamAccounts{group: "aimee-webchat", users: newFakeUsers()}
	err := p.Create("operator", "secret")
	if err == nil || !strings.Contains(err.Error(), "already exists") || !strings.Contains(err.Error(), "exit status 9") {
		t.Fatalf("Create error = %v; want useradd diagnostic and exit status", err)
	}
}

func TestCreateWithoutGroupUsesManagedUserInterface(t *testing.T) {
	users := newFakeUsers()
	p := &pamAccounts{users: users}
	if err := p.Create("operator", "secret"); err != nil {
		t.Fatal(err)
	}
	if users.created["operator"] != "secret" {
		t.Fatalf("fallback Create did not reach managed user interface: %#v", users.created)
	}
}
