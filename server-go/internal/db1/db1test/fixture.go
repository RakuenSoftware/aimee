// Package db1test starts the real PostgreSQL-backed store modules for Go tests.
//
// The workflow engine's store is the module now, so a test that needs a store
// needs one. There is no in-process substitute on purpose: a Go reimplementation
// of the operations would be a second copy of the semantics this port existed to
// centralise, and the first time the two disagreed the tests would be certifying
// the copy.
//
// What that costs is honest: these tests need the daemon, the Go multicall
// module binary, and a PostgreSQL database named by AIMEE_TEST_STORE_URL. When
// they are absent the fixture SKIPS rather than failing, so `go test ./...` in a
// Go-only checkout still passes and says why it did less.
//
// Layout per fixture: a temp home, a grant for DB1 and one for this process as
// the WFE principal, the daemon started on that home, the module attached to
// it, and a bus client for the test.
package db1test

import (
	"context"
	"crypto/sha256"
	"fmt"
	"io"
	"os"
	"os/exec"
	"path/filepath"
	"strconv"
	"strings"
	"sync"
	"testing"
	"time"

	"github.com/JBailes/aimee/server-go/bus"
	wire "github.com/JBailes/aimee/server-go/db1"
	"github.com/JBailes/aimee/server-go/internal/db1"
	"github.com/jackc/pgx/v5"
	"github.com/jackc/pgx/v5/pgxpool"
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
	serverEnv          = "AIMEE_TEST_SERVER_BIN"
	multicallModuleEnv = "AIMEE_TEST_MODULE_BIN"
	configEnv          = "AIMEE_TEST_CONFIG_MODULE_BIN"
	storeURLEnv        = "AIMEE_TEST_STORE_URL"
)

type fixture struct {
	home           string
	server         *exec.Cmd
	storeModule    *exec.Cmd
	postgresModule *exec.Cmd
	config         *exec.Cmd
	cancel         context.CancelFunc
	adminPool      *pgxpool.Pool
	storePool      *pgxpool.Pool
	schema         string
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

// locate finds the binaries relative to the repo root, which is three
// levels up from this package in a normal checkout.
func locate() (server string, multicallModule string, config string, err error) {
	if fromEnv := os.Getenv(serverEnv); fromEnv != "" {
		server = fromEnv
	}
	if fromEnv := os.Getenv(multicallModuleEnv); fromEnv != "" {
		multicallModule = fromEnv
	}
	if fromEnv := os.Getenv(configEnv); fromEnv != "" {
		config = fromEnv
	}
	if server != "" && multicallModule != "" && config != "" {
		return server, multicallModule, config, nil
	}
	root, err := repoRoot()
	if err != nil {
		return "", "", "", err
	}
	if server == "" {
		server = filepath.Join(root, "aimee-server")
	}
	if multicallModule == "" {
		multicallModule = filepath.Join(root, "src", "build", "obj", "aimee-module")
	}
	if config == "" {
		config = filepath.Join(root, "src", "build", "obj", "aimee-module-config")
	}
	for _, path := range []string{server, multicallModule, config} {
		if info, statErr := os.Stat(path); statErr != nil || info.IsDir() {
			return "", "", "", fmt.Errorf("%s is not built", path)
		}
	}
	return server, multicallModule, config, nil
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

// Open returns a store backed by the real modules, keyed by a caller-supplied
// fixture identity.
//
// The identity is still commonly a temp path because that keeps old test call
// sites simple, but no file is opened. A second Open with the same identity
// attaches to the modules already serving that isolated PostgreSQL schema.
func Open(t testing.TB, path string) (*db1.Store, error) {
	t.Helper()
	once.Do(func() {
		if _, _, _, err := locate(); err != nil {
			skipCause = err.Error()
		} else if os.Getenv(storeURLEnv) == "" {
			skipCause = storeURLEnv + " is unset"
		}
	})
	if skipCause != "" {
		t.Skipf("PostgreSQL store fixture unavailable: %s "+
			"(build it with `make -C src build/obj/aimee-module build/obj/aimee-module-config ../aimee-server`)", skipCause)
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
	serverBin, multicallModuleBin, configBin, err := locate()
	if err != nil {
		return nil, err
	}
	adminPool, storePool, storeURL, schema, err := prepareStore(t, os.Getenv(storeURLEnv), path)
	if err != nil {
		return nil, err
	}
	cleanupStore := func() {
		storePool.Close()
		_, _ = adminPool.Exec(context.Background(), "DROP SCHEMA "+pgx.Identifier{schema}.Sanitize()+" CASCADE")
		adminPool.Close()
	}
	keepStore := false
	defer func() {
		if !keepStore {
			cleanupStore()
		}
	}()
	home, err := os.MkdirTemp("", "db1test-")
	if err != nil {
		return nil, err
	}
	keepHome := false
	defer func() {
		if !keepHome {
			_ = os.RemoveAll(home)
		}
	}()
	aimeeHome := filepath.Join(home, ".config", "aimee")
	moduleBinDir := filepath.Join(home, "modules")
	if err := os.MkdirAll(moduleBinDir, 0o700); err != nil {
		return nil, err
	}
	storeModuleBin := filepath.Join(moduleBinDir, "aimee-module-aimee")
	postgresModuleBin := filepath.Join(moduleBinDir, "aimee-module-postgres")
	for _, destination := range []string{storeModuleBin, postgresModuleBin} {
		if err := copyExecutable(multicallModuleBin, destination); err != nil {
			return nil, err
		}
	}
	policy := filepath.Join(aimeeHome, "modules.d", "server")
	if err := os.MkdirAll(policy, 0o700); err != nil {
		return nil, err
	}
	// The grants the supervisor would write. The module serves DB1's kinds; this
	// test process attaches as the WFE principal, which is what the engine does
	// in production.
	if err := writeGrant(filepath.Join(policy, "aimee.grant"), 1, 30, storeModuleBin,
		serveKinds()); err != nil {
		return nil, err
	}
	if err := writeGrant(filepath.Join(policy, "postgres.grant"), 1, 28,
		postgresModuleBin, "11265,11266"); err != nil {
		return nil, err
	}
	if err := writeGrantRequesting(filepath.Join(policy, "aimee-postgres.grant"), 1, 69,
		storeModuleBin, 11266); err != nil {
		return nil, err
	}
	if err := writeGrant(filepath.Join(policy, "config.grant"), 1, 2, configBin, "4609"); err != nil {
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
	busSock := filepath.Join(aimeeHome, "server-module-bus.sock")
	if err := waitFor(busSock, 10*time.Second); err != nil {
		cancel()
		return nil, fmt.Errorf("daemon module bus did not bind: %w\nserver: %s", err,
			tail(filepath.Join(home, "server.log")))
	}
	configModule := exec.CommandContext(ctx, configBin, busSock)
	configModule.Env = append(os.Environ(), "HOME="+home, "AIMEE_HOME="+aimeeHome)
	configLog, _ := os.Create(filepath.Join(home, "config.log"))
	configModule.Stdout, configModule.Stderr = configLog, configLog
	if err := configModule.Start(); err != nil {
		cancel()
		return nil, err
	}
	httpSock := filepath.Join(aimeeHome, "aimee-http.sock")
	if err := waitFor(httpSock, 30*time.Second); err != nil {
		cancel()
		return nil, fmt.Errorf("daemon did not bind: %w\nserver: %s\nconfig: %s", err,
			tail(filepath.Join(home, "server.log")), tail(filepath.Join(home, "config.log")))
	}

	postgresModule := exec.CommandContext(ctx, postgresModuleBin, busSock)
	postgresModule.Env = append(os.Environ(), "HOME="+home, "AIMEE_HOME="+aimeeHome,
		"AIMEE_STORE_URL="+storeURL)
	postgresLog, _ := os.Create(filepath.Join(home, "postgres.log"))
	postgresModule.Stdout, postgresModule.Stderr = postgresLog, postgresLog
	if err := postgresModule.Start(); err != nil {
		cancel()
		return nil, err
	}

	storeModule := exec.CommandContext(ctx, storeModuleBin, busSock)
	storeModule.Env = append(os.Environ(), "HOME="+home, "AIMEE_HOME="+aimeeHome)
	storeLog, _ := os.Create(filepath.Join(home, "store.log"))
	storeModule.Stdout, storeModule.Stderr = storeLog, storeLog
	if err := storeModule.Start(); err != nil {
		cancel()
		return nil, err
	}

	f := &fixture{
		home: home, server: server, storeModule: storeModule, postgresModule: postgresModule,
		config: configModule, cancel: cancel, adminPool: adminPool, storePool: storePool, schema: schema,
	}
	t.Cleanup(func() {
		mu.Lock()
		defer mu.Unlock()
		if fixtures[path] == f {
			delete(fixtures, path)
			f.cancel()
			for _, cmd := range []*exec.Cmd{f.storeModule, f.postgresModule, f.config, f.server} {
				if cmd != nil && cmd.Process != nil {
					_ = cmd.Process.Kill()
					_ = cmd.Wait()
				}
			}
			f.storePool.Close()
			_, _ = f.adminPool.Exec(context.Background(),
				"DROP SCHEMA "+pgx.Identifier{f.schema}.Sanitize()+" CASCADE")
			f.adminPool.Close()
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
			keepStore = true
			keepHome = true
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
		lastErr, names, tail(filepath.Join(home, "server.log")), tail(filepath.Join(home, "store.log")))
}

func copyExecutable(source, destination string) error {
	in, err := os.Open(source)
	if err != nil {
		return fmt.Errorf("open module multicall binary: %w", err)
	}
	defer in.Close()
	out, err := os.OpenFile(destination, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0o700)
	if err != nil {
		return fmt.Errorf("create module alias %s: %w", destination, err)
	}
	if _, err := io.Copy(out, in); err != nil {
		_ = out.Close()
		return fmt.Errorf("copy module alias %s: %w", destination, err)
	}
	if err := out.Close(); err != nil {
		return fmt.Errorf("close module alias %s: %w", destination, err)
	}
	return nil
}

func prepareStore(t testing.TB, baseURL, key string) (*pgxpool.Pool, *pgxpool.Pool, string, string, error) {
	t.Helper()
	ctx, cancel := context.WithTimeout(context.Background(), 10*time.Second)
	defer cancel()
	adminConfig, err := pgxpool.ParseConfig(baseURL)
	if err != nil {
		return nil, nil, "", "", fmt.Errorf("parse %s: %w", storeURLEnv, err)
	}
	adminPool, err := pgxpool.NewWithConfig(ctx, adminConfig)
	if err != nil {
		return nil, nil, "", "", fmt.Errorf("open test PostgreSQL: %w", err)
	}
	if err := adminPool.Ping(ctx); err != nil {
		adminPool.Close()
		return nil, nil, "", "", fmt.Errorf("ping test PostgreSQL: %w", err)
	}
	digest := sha256.Sum256([]byte(key + strconv.FormatInt(time.Now().UnixNano(), 10)))
	schema := fmt.Sprintf("aimee_store_test_%x", digest[:8])
	if _, err := adminPool.Exec(ctx, "CREATE SCHEMA "+pgx.Identifier{schema}.Sanitize()); err != nil {
		adminPool.Close()
		return nil, nil, "", "", fmt.Errorf("create isolated test schema: %w", err)
	}
	storeConfig, err := pgxpool.ParseConfig(baseURL)
	if err != nil {
		adminPool.Close()
		return nil, nil, "", "", err
	}
	storeConfig.ConnConfig.RuntimeParams["search_path"] = schema
	storePool, err := pgxpool.NewWithConfig(ctx, storeConfig)
	if err != nil {
		_, _ = adminPool.Exec(context.Background(), "DROP SCHEMA "+pgx.Identifier{schema}.Sanitize()+" CASCADE")
		adminPool.Close()
		return nil, nil, "", "", fmt.Errorf("open isolated test schema: %w", err)
	}
	return adminPool, storePool, storeConfig.ConnConfig.ConnString(), schema, nil
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

// Exec runs a statement directly against the fixture's isolated PostgreSQL schema.
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
	store := fixtureStore(t, path)
	if _, err := store.Exec(context.Background(), rebind(query), args...); err != nil {
		t.Fatalf("test setup statement failed: %v", err)
	}
}

// QueryInt reads one integer directly from the store, for assertions about
// columns no operation exposes. Same caveat as Exec.
func QueryInt(t testing.TB, path, query string, args ...any) int {
	t.Helper()
	store := fixtureStore(t, path)
	var out int
	if err := store.QueryRow(context.Background(), rebind(query), args...).Scan(&out); err != nil {
		t.Fatalf("test assertion query failed: %v", err)
	}
	return out
}

func fixtureStore(t testing.TB, path string) *pgxpool.Pool {
	t.Helper()
	mu.Lock()
	defer mu.Unlock()
	f, ok := fixtures[path]
	if !ok || f.storePool == nil {
		t.Fatalf("no PostgreSQL store fixture for %s", path)
	}
	return f.storePool
}

// rebind preserves the test helpers' database/sql-style placeholders while the
// real PostgreSQL driver receives the numbered placeholders it requires.
func rebind(query string) string {
	var out strings.Builder
	out.Grow(len(query) + 8)
	argument := 1
	quoted := false
	for i := 0; i < len(query); i++ {
		switch query[i] {
		case '\'':
			out.WriteByte(query[i])
			if quoted && i+1 < len(query) && query[i+1] == '\'' {
				i++
				out.WriteByte(query[i])
				continue
			}
			quoted = !quoted
		case '?':
			if quoted {
				out.WriteByte(query[i])
			} else {
				out.WriteByte('$')
				out.WriteString(strconv.Itoa(argument))
				argument++
			}
		default:
			out.WriteByte(query[i])
		}
	}
	return out.String()
}

// Client returns the bus client itself, for tests about the generated wire
// surface rather than about the store's behaviour.
//
// Most tests want a Store: it is the engine's vocabulary and the thing the
// engine uses. A few want the layer underneath -- an operation the Store does
// not expose, or a reply SHAPE whose handling is generated and therefore worth
// testing once on its own.
func Client(t testing.TB, path string) *wire.Client {
	t.Helper()
	if _, err := Open(t, path); err != nil {
		t.Fatalf("start module: %v", err)
	}
	mu.Lock()
	defer mu.Unlock()
	existing, ok := fixtures[path]
	if !ok || existing.client == nil {
		t.Fatalf("no client for %s", path)
	}
	return existing.client
}
