package workflowstoretest

import (
	"os"
	"path/filepath"
	"testing"

	"github.com/jackc/pgx/v5/pgxpool"
)

func TestChildDSNOverridesIdentityAndSchema(t *testing.T) {
	for _, base := range []string{
		"postgresql://old:old-password@localhost/db?search_path=public&sslmode=disable",
		"host=localhost dbname=db user=old password=old-password search_path=public sslmode=disable",
	} {
		params := map[string]string{"user": "runtime", "password": `quote'and\slash`, "search_path": "isolated_fixture"}
		config, err := pgxpool.ParseConfig(withDSNParams(base, params))
		if err != nil {
			t.Fatal("generated child DSN is invalid")
		}
		if config.ConnConfig.User != params["user"] || config.ConnConfig.Password != params["password"] || config.ConnConfig.RuntimeParams["search_path"] != params["search_path"] {
			t.Fatal("child did not receive isolated identity and schema")
		}
	}
}

func TestRebindNumbersOnlyUnquotedPlaceholders(t *testing.T) {
	t.Parallel()
	query := `UPDATE jobs SET detail='literal ? and ''quoted ?''' WHERE id=? AND owner=?`
	want := `UPDATE jobs SET detail='literal ? and ''quoted ?''' WHERE id=$1 AND owner=$2`
	if got := rebind(query); got != want {
		t.Fatalf("rebind=%q want %q", got, want)
	}
}

func TestCopyExecutableCreatesPrivateModuleAlias(t *testing.T) {
	t.Parallel()
	dir := t.TempDir()
	source := filepath.Join(dir, "aimee-module")
	destination := filepath.Join(dir, "aimee-module-postgres")
	if err := os.WriteFile(source, []byte("module"), 0o700); err != nil {
		t.Fatal(err)
	}
	if err := copyExecutable(source, destination); err != nil {
		t.Fatal(err)
	}
	body, err := os.ReadFile(destination)
	if err != nil {
		t.Fatal(err)
	}
	if string(body) != "module" {
		t.Fatalf("alias body=%q", body)
	}
	info, err := os.Stat(destination)
	if err != nil {
		t.Fatal(err)
	}
	if got := info.Mode().Perm(); got != 0o700 {
		t.Fatalf("alias mode=%#o want 0700", got)
	}
}

func TestStartupReconciledRequiresActualSuccessMarker(t *testing.T) {
	path := filepath.Join(t.TempDir(), "server.log")
	if startupReconciled(path) {
		t.Fatal("missing log admitted fixture seeds")
	}
	for _, body := range []string{
		"WARN  server: delegate startup reconciliation pending store attachment; new jobs blocked\n",
		"INFO  coord_dispatcher: started\n",
		"INFO  server: delegate startup reconciliation complete",
	} {
		if err := os.WriteFile(path, []byte(body), 0600); err != nil {
			t.Fatal(err)
		}
		if startupReconciled(path) {
			t.Fatal("unacknowledged/incomplete startup marker admitted fixture seeds")
		}
	}
	if err := os.WriteFile(path, []byte("2026-10-22T00:00:00Z INFO  server: delegate startup reconciliation complete\n"), 0600); err != nil {
		t.Fatal(err)
	}
	if !startupReconciled(path) {
		t.Fatal("acknowledged daemon startup not ready")
	}
}
