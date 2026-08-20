// Package db1test starts a real DB1 module for Go tests.
//
// The workflow engine's store is the module now, so a test that needs a store
// needs one. There is no in-process substitute on purpose: a Go reimplementation
// of the operations would be a second copy of the semantics this port existed to
// centralise, and the first time the two disagreed the tests would be certifying
// the copy.
//
// What that costs is honest: these tests need two binaries that come from the C
// build -- the daemon, which hosts the module bus, and the module itself. When
// they are absent the fixture SKIPS rather than failing, so `go test ./...` in a
// Go-only checkout still passes and says why it did less.
//
// Layout per fixture: a temp home, a grant for DB1 and one for this process as
// the WFE principal, the daemon started on that home, the module attached to
// it, and a bus client for the test.
package db1test

import (
	"context"
	"database/sql"
	"fmt"
	"os"
	"os/exec"
	"path/filepath"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
	"github.com/JBailes/aimee/server-go/internal/db1"

	_ "modernc.org/sqlite"
)

// The WFE's bus identity, spelled here rather than imported from internal/engine
// because engine's own tests use this fixture and the import would be a cycle.
// They must agree with busPrincipalClass and wfeBusPrincipalRef;
// a mismatch shows up immediately as an attach the daemon refuses.
const (
	busPrincipalClass  uint32 = 1
	wfeBusPrincipalRef uint32 = 64
)

// Binaries the fixture needs, overridable so a CI job that builds elsewhere can
// point at them without moving files around.
const (
	serverEnv = "AIMEE_TEST_SERVER_BIN"
	moduleEnv = "AIMEE_TEST_DB1_MODULE_BIN"
)

type fixture struct {
	home   string
	server *exec.Cmd
	module *exec.Cmd
	cancel context.CancelFunc
	// One client per fixture, deliberately. The bus denies a second live attach
	// for the same principal -- correctly, since two live slots for one identity
	// is how a stale one gets reaped in favour of an impostor -- so every store
	// handed out for this store shares the attachment the engine would have.
	client *wire.Client
}

var (
	mu        sync.Mutex
	fixtures  = map[string]*fixture{}
	skipCause string
	once      sync.Once
)

// locate finds the two binaries relative to the repo root, which is three
// levels up from this package in a normal checkout.
func locate() (server string, module string, err error) {
	if fromEnv := os.Getenv(serverEnv); fromEnv != "" {
		server = fromEnv
	}
	if fromEnv := os.Getenv(moduleEnv); fromEnv != "" {
		module = fromEnv
	}
	if server != "" && module != "" {
		return server, module, nil
	}
	root, err := repoRoot()
	if err != nil {
		return "", "", err
	}
	if server == "" {
		server = filepath.Join(root, "aimee-server")
	}
	if module == "" {
		module = filepath.Join(root, "src", "build", "obj", "aimee-module-db1")
	}
	for _, path := range []string{server, module} {
		if info, statErr := os.Stat(path); statErr != nil || info.IsDir() {
			return "", "", fmt.Errorf("%s is not built", path)
		}
	}
	return server, module, nil
}

func repoRoot() (string, error) {
	dir, err := os.Getwd()
	if err != nil {
		return "", err
	}
	for i := 0; i < 8; i++ {
		if _, err := os.Stat(filepath.Join(dir, "server-go", "go.mod")); err == nil {
			return dir, nil
		}
		parent := filepath.Dir(dir)
		if parent == dir {
			break
		}
		dir = parent
	}
	return "", fmt.Errorf("repository root not found from the test's working directory")
}

// Open returns a store backed by a real module, keyed by path.
//
// Keying by path is what preserves the tests that reopen the same "database" to
// simulate a restart: a second Open on the same path attaches to the module that
// is already holding that store, so the data is still there -- which is exactly
// what reopening a file used to mean.
func Open(t testing.TB, path string) (*db1.Store, error) {
	t.Helper()
	once.Do(func() {
		if _, _, err := locate(); err != nil {
			skipCause = err.Error()
		}
	})
	if skipCause != "" {
		t.Skipf("DB1 module fixture unavailable: %s "+
			"(build it with `make -C src build/obj/aimee-module-db1 ../aimee-server`)", skipCause)
	}
	mu.Lock()
	defer mu.Unlock()
	existing, ok := fixtures[path]
	if !ok {
		started, err := start(t, path)
		if err != nil {
			return nil, err
		}
		fixtures[path] = started
		existing = started
	}
	return db1.OpenBus(existing.client)
}

func start(t testing.TB, path string) (*fixture, error) {
	t.Helper()
	serverBin, moduleBin, err := locate()
	if err != nil {
		return nil, err
	}
	home, err := os.MkdirTemp("", "db1test-")
	if err != nil {
		return nil, err
	}
	aimeeHome := filepath.Join(home, ".config", "aimee")
	policy := filepath.Join(aimeeHome, "modules.d", "server")
	if err := os.MkdirAll(policy, 0o700); err != nil {
		return nil, err
	}
	// The grants the supervisor would write. The module serves DB1's kinds; this
	// test process attaches as the WFE principal, which is what the engine does
	// in production.
	if err := writeGrant(filepath.Join(policy, "db1.grant"), 1, 30, moduleBin,
		serveKinds()); err != nil {
		return nil, err
	}
	// Mirrors the wfe client in src/modules/process-contracts.json: the engine
	// requests DB1's lifecycle kind, which is what makes its store reachable.
	//
	// The executable has to be the path the daemon will see for this process --
	// it reads the peer's own /proc entry and compares -- so it is resolved the
	// same way rather than taken from argv, which a test runner does not
	// guarantee to be a resolved absolute path.
	self, err := os.Executable()
	if err != nil {
		return nil, err
	}
	if resolved, linkErr := filepath.EvalSymlinks(self); linkErr == nil {
		self = resolved
	}
	if err := writeGrantRequesting(filepath.Join(policy, "wfe.grant"), busPrincipalClass,
		wfeBusPrincipalRef, self, lifecycleKind); err != nil {
		return nil, err
	}

	ctx, cancel := context.WithCancel(context.Background())
	server := exec.CommandContext(ctx, serverBin, "--foreground")
	server.Env = append(os.Environ(), "HOME="+home, "AIMEE_HOME="+aimeeHome)
	serverLog, _ := os.Create(filepath.Join(home, "server.log"))
	server.Stdout, server.Stderr = serverLog, serverLog
	if err := server.Start(); err != nil {
		cancel()
		return nil, err
	}
	httpSock := filepath.Join(aimeeHome, "aimee-http.sock")
	if err := waitFor(httpSock, 30*time.Second); err != nil {
		cancel()
		return nil, fmt.Errorf("daemon did not bind: %w", err)
	}

	busSock := filepath.Join(aimeeHome, "server-module-bus.sock")
	module := exec.CommandContext(ctx, moduleBin, busSock)
	module.Env = append(os.Environ(), "HOME="+home, "AIMEE_DB1_PATH="+path)
	moduleLog, _ := os.Create(filepath.Join(home, "module.log"))
	module.Stdout, module.Stderr = moduleLog, moduleLog
	if err := module.Start(); err != nil {
		cancel()
		return nil, err
	}

	f := &fixture{home: home, server: server, module: module, cancel: cancel}
	t.Cleanup(func() {
		mu.Lock()
		defer mu.Unlock()
		if fixtures[path] == f {
			delete(fixtures, path)
			f.cancel()
			_ = os.RemoveAll(f.home)
		}
	})
	// The module attaches on its own schedule; wait for it to be serving before
	// handing the test a client, so a slow attach is a wait rather than a
	// mysterious first-call failure.
	// Attach once, then wait for the module to answer through it. Attaching on
	// every poll would burn a slot per attempt and deny the one that mattered.
	deadline := time.Now().Add(20 * time.Second)
	var lastErr error
	for time.Now().Before(deadline) && f.client == nil {
		client, err := attach(f)
		if err == nil {
			f.client = client
			break
		}
		lastErr = fmt.Errorf("attach: %w", err)
		time.Sleep(100 * time.Millisecond)
	}
	for f.client != nil && time.Now().Before(deadline) {
		if _, callErr := f.client.WfeActiveRootCount(context.Background()); callErr == nil {
			return f, nil
		} else {
			lastErr = fmt.Errorf("call: %w", callErr)
		}
		time.Sleep(100 * time.Millisecond)
	}
	cancel()
	entries, _ := os.ReadDir(aimeeHome)
	names := ""
	for _, e := range entries {
		names += " " + e.Name()
	}
	return nil, fmt.Errorf("module did not start serving within 20s: %v\nhome:%s\nserver: %s\nmodule: %s",
		lastErr, names, tail(filepath.Join(home, "server.log")), tail(filepath.Join(home, "module.log")))
}

func attach(f *fixture) (*wire.Client, error) {
	ctx := context.Background()
	busSock := filepath.Join(f.home, ".config", "aimee", "server-module-bus.sock")
	attached, err := bus.ConnectClient(ctx, busSock, busPrincipalClass,
		wfeBusPrincipalRef)
	if err != nil {
		return nil, err
	}
	caller, err := bus.NewConcurrentModuleCaller(ctx, attached)
	if err != nil {
		return nil, err
	}
	return wire.NewClient(caller, 0)
}

// lifecycleKind is DB1's lifecycle stage, the one the engine's store calls.
const lifecycleKind = 11792

func writeGrantRequesting(path string, class, ref uint32, executable string, request int) error {
	body := fmt.Sprintf("version=1\nprincipal_class=%d\nprincipal_ref=%d\nuid=self\n"+
		"executable=%s\npublish=\nsubscribe=\nrequest=%d\nserve=\n",
		class, ref, executable, request)
	return os.WriteFile(path, []byte(body), 0o600)
}

func writeGrant(path string, class, ref uint32, executable, serve string) error {
	body := fmt.Sprintf("version=1\nprincipal_class=%d\nprincipal_ref=%d\nuid=self\n"+
		"executable=%s\npublish=\nsubscribe=\nrequest=\nserve=%s\n",
		class, ref, executable, serve)
	return os.WriteFile(path, []byte(body), 0o600)
}

// serveKinds lists DB1's event kinds. Derived from the family base rather than
// spelled out, so a family added to the catalog does not have to be remembered
// here as well.
func serveKinds() string {
	const base = 11776
	const families = 19
	out := ""
	for i := 1; i <= families; i++ {
		if out != "" {
			out += ","
		}
		out += fmt.Sprintf("%d", base+i)
	}
	return out
}

// tail returns the last of a process log, for a failure that would otherwise
// say only that nothing happened.
func tail(path string) string {
	body, err := os.ReadFile(path)
	if err != nil {
		return "(no log: " + err.Error() + ")"
	}
	if len(body) > 1500 {
		body = body[len(body)-1500:]
	}
	return string(body)
}

func waitFor(path string, within time.Duration) error {
	deadline := time.Now().Add(within)
	for time.Now().Before(deadline) {
		if _, err := os.Stat(path); err == nil {
			return nil
		}
		time.Sleep(50 * time.Millisecond)
	}
	return fmt.Errorf("%s did not appear", path)
}

// Exec runs a statement directly against the store's file.
//
// This is a side door and it is here for one reason: some behaviour is only
// reachable by moving the clock. A budget lease lasts two minutes, and a test
// that waits two minutes is a test nobody runs. Expiring the lease by hand is
// the only practical way to exercise takeover, replay-only admission and the
// paths that depend on them.
//
// It is NOT a second writer in the sense the port removed. The engine reaches
// the store through the module; this reaches around it deliberately, in a test,
// to simulate elapsed time -- and it is in package db1test rather than anywhere
// a production caller could reach it.
func Exec(t testing.TB, path, query string, args ...any) {
	t.Helper()
	handle, err := sql.Open("sqlite", "file:"+path+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open store for test setup: %v", err)
	}
	defer handle.Close()
	if _, err := handle.ExecContext(context.Background(), query, args...); err != nil {
		t.Fatalf("test setup statement failed: %v", err)
	}
}

// QueryInt reads one integer directly from the store, for assertions about
// columns no operation exposes. Same caveat as Exec.
func QueryInt(t testing.TB, path, query string, args ...any) int {
	t.Helper()
	handle, err := sql.Open("sqlite", "file:"+path+"?_pragma=busy_timeout(5000)")
	if err != nil {
		t.Fatalf("open store for test assertion: %v", err)
	}
	defer handle.Close()
	var out int
	if err := handle.QueryRowContext(context.Background(), query, args...).Scan(&out); err != nil {
		t.Fatalf("test assertion query failed: %v", err)
	}
	return out
}
